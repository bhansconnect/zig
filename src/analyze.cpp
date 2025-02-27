﻿/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "analyze.hpp"
#include "ast_render.hpp"
#include "codegen.hpp"
#include "config.h"
#include "error.hpp"
#include "ir.hpp"
#include "ir_print.hpp"
#include "os.hpp"
#include "parser.hpp"
#include "softfloat.hpp"
#include "zig_llvm.h"


static const size_t default_backward_branch_quota = 1000;

static Error ATTRIBUTE_MUST_USE resolve_struct_type(CodeGen *g, ZigType *struct_type);

static Error ATTRIBUTE_MUST_USE resolve_struct_zero_bits(CodeGen *g, ZigType *struct_type);
static Error ATTRIBUTE_MUST_USE resolve_struct_alignment(CodeGen *g, ZigType *struct_type);
static Error ATTRIBUTE_MUST_USE resolve_enum_zero_bits(CodeGen *g, ZigType *enum_type);
static Error ATTRIBUTE_MUST_USE resolve_union_zero_bits(CodeGen *g, ZigType *union_type);
static Error ATTRIBUTE_MUST_USE resolve_union_alignment(CodeGen *g, ZigType *union_type);
static void analyze_fn_body(CodeGen *g, ZigFn *fn_table_entry);
static void resolve_llvm_types(CodeGen *g, ZigType *type, ResolveStatus wanted_resolve_status);
static void preview_use_decl(CodeGen *g, TldUsingNamespace *using_namespace, ScopeDecls *dest_decls_scope);
static void resolve_use_decl(CodeGen *g, TldUsingNamespace *tld_using_namespace, ScopeDecls *dest_decls_scope);
static void analyze_fn_async(CodeGen *g, ZigFn *fn, bool resolve_frame);

// nullptr means not analyzed yet; this one means currently being analyzed
static const AstNode *inferred_async_checking = reinterpret_cast<AstNode *>(0x1);
// this one means analyzed and it's not async
static const AstNode *inferred_async_none = reinterpret_cast<AstNode *>(0x2);

static bool is_top_level_struct(ZigType *import) {
    return import->id == ZigTypeIdStruct && import->data.structure.root_struct != nullptr;
}

static ErrorMsg *add_error_note_token(CodeGen *g, ErrorMsg *parent_msg, ZigType *owner, Token *token, Buf *msg) {
    assert(is_top_level_struct(owner));
    RootStruct *root_struct = owner->data.structure.root_struct;

    ErrorMsg *err = err_msg_create_with_line(root_struct->path, token->start_line, token->start_column,
            root_struct->source_code, root_struct->line_offsets, msg);

    err_msg_add_note(parent_msg, err);
    return err;
}

ErrorMsg *add_token_error(CodeGen *g, ZigType *owner, Token *token, Buf *msg) {
    assert(is_top_level_struct(owner));
    RootStruct *root_struct = owner->data.structure.root_struct;
    ErrorMsg *err = err_msg_create_with_line(root_struct->path, token->start_line, token->start_column,
            root_struct->source_code, root_struct->line_offsets, msg);

    g->errors.append(err);
    g->trace_err = err;
    return err;
}

ErrorMsg *add_node_error(CodeGen *g, AstNode *node, Buf *msg) {
    Token fake_token;
    fake_token.start_line = node->line;
    fake_token.start_column = node->column;
    node->already_traced_this_node = true;
    return add_token_error(g, node->owner, &fake_token, msg);
}

ErrorMsg *add_error_note(CodeGen *g, ErrorMsg *parent_msg, const AstNode *node, Buf *msg) {
    Token fake_token;
    fake_token.start_line = node->line;
    fake_token.start_column = node->column;
    return add_error_note_token(g, parent_msg, node->owner, &fake_token, msg);
}

ZigType *new_type_table_entry(ZigTypeId id) {
    ZigType *entry = allocate<ZigType>(1);
    entry->id = id;
    return entry;
}

static ScopeDecls **get_container_scope_ptr(ZigType *type_entry) {
    if (type_entry->id == ZigTypeIdStruct) {
        return &type_entry->data.structure.decls_scope;
    } else if (type_entry->id == ZigTypeIdEnum) {
        return &type_entry->data.enumeration.decls_scope;
    } else if (type_entry->id == ZigTypeIdUnion) {
        return &type_entry->data.unionation.decls_scope;
    }
    zig_unreachable();
}

static ScopeExpr *find_expr_scope(Scope *scope) {
    for (;;) {
        switch (scope->id) {
            case ScopeIdExpr:
                return reinterpret_cast<ScopeExpr *>(scope);
            case ScopeIdDefer:
            case ScopeIdDeferExpr:
            case ScopeIdDecls:
            case ScopeIdFnDef:
            case ScopeIdCompTime:
            case ScopeIdVarDecl:
            case ScopeIdCImport:
            case ScopeIdSuspend:
            case ScopeIdTypeOf:
            case ScopeIdBlock:
                return nullptr;
            case ScopeIdLoop:
            case ScopeIdRuntime:
                scope = scope->parent;
                continue;
        }
    }
}

static void update_progress_display(CodeGen *g) {
    stage2_progress_update_node(g->sub_progress_node, 
        g->resolve_queue_index + g->fn_defs_index,
        g->resolve_queue.length + g->fn_defs.length);
}

ScopeDecls *get_container_scope(ZigType *type_entry) {
    return *get_container_scope_ptr(type_entry);
}

void init_scope(CodeGen *g, Scope *dest, ScopeId id, AstNode *source_node, Scope *parent) {
    dest->codegen = g;
    dest->id = id;
    dest->source_node = source_node;
    dest->parent = parent;
}

static ScopeDecls *create_decls_scope(CodeGen *g, AstNode *node, Scope *parent, ZigType *container_type,
        ZigType *import, Buf *bare_name)
{
    assert(node == nullptr || node->type == NodeTypeContainerDecl || node->type == NodeTypeFnCallExpr);
    ScopeDecls *scope = allocate<ScopeDecls>(1);
    init_scope(g, &scope->base, ScopeIdDecls, node, parent);
    scope->decl_table.init(4);
    scope->container_type = container_type;
    scope->import = import;
    scope->bare_name = bare_name;
    return scope;
}

ScopeBlock *create_block_scope(CodeGen *g, AstNode *node, Scope *parent) {
    assert(node->type == NodeTypeBlock);
    ScopeBlock *scope = allocate<ScopeBlock>(1);
    init_scope(g, &scope->base, ScopeIdBlock, node, parent);
    scope->name = node->data.block.name;
    return scope;
}

ScopeDefer *create_defer_scope(CodeGen *g, AstNode *node, Scope *parent) {
    assert(node->type == NodeTypeDefer);
    ScopeDefer *scope = allocate<ScopeDefer>(1);
    init_scope(g, &scope->base, ScopeIdDefer, node, parent);
    return scope;
}

ScopeDeferExpr *create_defer_expr_scope(CodeGen *g, AstNode *node, Scope *parent) {
    assert(node->type == NodeTypeDefer);
    ScopeDeferExpr *scope = allocate<ScopeDeferExpr>(1);
    init_scope(g, &scope->base, ScopeIdDeferExpr, node, parent);
    return scope;
}

Scope *create_var_scope(CodeGen *g, AstNode *node, Scope *parent, ZigVar *var) {
    ScopeVarDecl *scope = allocate<ScopeVarDecl>(1);
    init_scope(g, &scope->base, ScopeIdVarDecl, node, parent);
    scope->var = var;
    return &scope->base;
}

ScopeCImport *create_cimport_scope(CodeGen *g, AstNode *node, Scope *parent) {
    assert(node->type == NodeTypeFnCallExpr);
    ScopeCImport *scope = allocate<ScopeCImport>(1);
    init_scope(g, &scope->base, ScopeIdCImport, node, parent);
    buf_resize(&scope->buf, 0);
    return scope;
}

ScopeLoop *create_loop_scope(CodeGen *g, AstNode *node, Scope *parent) {
    ScopeLoop *scope = allocate<ScopeLoop>(1);
    init_scope(g, &scope->base, ScopeIdLoop, node, parent);
    if (node->type == NodeTypeWhileExpr) {
        scope->name = node->data.while_expr.name;
    } else if (node->type == NodeTypeForExpr) {
        scope->name = node->data.for_expr.name;
    } else {
        zig_unreachable();
    }
    return scope;
}

Scope *create_runtime_scope(CodeGen *g, AstNode *node, Scope *parent, IrInstruction *is_comptime) {
    ScopeRuntime *scope = allocate<ScopeRuntime>(1);
    scope->is_comptime = is_comptime;
    init_scope(g, &scope->base, ScopeIdRuntime, node, parent);
    return &scope->base;
}

ScopeSuspend *create_suspend_scope(CodeGen *g, AstNode *node, Scope *parent) {
    assert(node->type == NodeTypeSuspend);
    ScopeSuspend *scope = allocate<ScopeSuspend>(1);
    init_scope(g, &scope->base, ScopeIdSuspend, node, parent);
    return scope;
}

ScopeFnDef *create_fndef_scope(CodeGen *g, AstNode *node, Scope *parent, ZigFn *fn_entry) {
    ScopeFnDef *scope = allocate<ScopeFnDef>(1);
    init_scope(g, &scope->base, ScopeIdFnDef, node, parent);
    scope->fn_entry = fn_entry;
    return scope;
}

Scope *create_comptime_scope(CodeGen *g, AstNode *node, Scope *parent) {
    ScopeCompTime *scope = allocate<ScopeCompTime>(1);
    init_scope(g, &scope->base, ScopeIdCompTime, node, parent);
    return &scope->base;
}

Scope *create_typeof_scope(CodeGen *g, AstNode *node, Scope *parent) {
    ScopeTypeOf *scope = allocate<ScopeTypeOf>(1);
    init_scope(g, &scope->base, ScopeIdTypeOf, node, parent);
    return &scope->base;
}

ScopeExpr *create_expr_scope(CodeGen *g, AstNode *node, Scope *parent) {
    ScopeExpr *scope = allocate<ScopeExpr>(1);
    init_scope(g, &scope->base, ScopeIdExpr, node, parent);
    ScopeExpr *parent_expr = find_expr_scope(parent);
    if (parent_expr != nullptr) {
        size_t new_len = parent_expr->children_len + 1;
        parent_expr->children_ptr = reallocate_nonzero<ScopeExpr *>(
                parent_expr->children_ptr, parent_expr->children_len, new_len);
        parent_expr->children_ptr[parent_expr->children_len] = scope;
        parent_expr->children_len = new_len;
    }
    return scope;
}

ZigType *get_scope_import(Scope *scope) {
    while (scope) {
        if (scope->id == ScopeIdDecls) {
            ScopeDecls *decls_scope = (ScopeDecls *)scope;
            assert(is_top_level_struct(decls_scope->import));
            return decls_scope->import;
        }
        scope = scope->parent;
    }
    zig_unreachable();
}

ScopeTypeOf *get_scope_typeof(Scope *scope) {
    while (scope) {
        switch (scope->id) {
            case ScopeIdTypeOf:
                return reinterpret_cast<ScopeTypeOf *>(scope);
            case ScopeIdFnDef:
            case ScopeIdDecls:
                return nullptr;
            default:
                scope = scope->parent;
                continue;
        }
    }
    zig_unreachable();
}

static ZigType *new_container_type_entry(CodeGen *g, ZigTypeId id, AstNode *source_node, Scope *parent_scope,
        Buf *bare_name)
{
    ZigType *entry = new_type_table_entry(id);
    *get_container_scope_ptr(entry) = create_decls_scope(g, source_node, parent_scope, entry,
            get_scope_import(parent_scope), bare_name);
    return entry;
}

static uint8_t bits_needed_for_unsigned(uint64_t x) {
    if (x == 0) {
        return 0;
    }
    uint8_t base = log2_u64(x);
    uint64_t upper = (((uint64_t)1) << base) - 1;
    return (upper >= x) ? base : (base + 1);
}

AstNode *type_decl_node(ZigType *type_entry) {
    switch (type_entry->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdStruct:
            return type_entry->data.structure.decl_node;
        case ZigTypeIdEnum:
            return type_entry->data.enumeration.decl_node;
        case ZigTypeIdUnion:
            return type_entry->data.unionation.decl_node;
        case ZigTypeIdFnFrame:
            return type_entry->data.frame.fn->proto_node;
        case ZigTypeIdOpaque:
        case ZigTypeIdMetaType:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdUnreachable:
        case ZigTypeIdInt:
        case ZigTypeIdFloat:
        case ZigTypeIdPointer:
        case ZigTypeIdArray:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdOptional:
        case ZigTypeIdErrorUnion:
        case ZigTypeIdErrorSet:
        case ZigTypeIdFn:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
        case ZigTypeIdVector:
        case ZigTypeIdAnyFrame:
            return nullptr;
    }
    zig_unreachable();
}

bool type_is_resolved(ZigType *type_entry, ResolveStatus status) {
    switch (type_entry->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdStruct:
            return type_entry->data.structure.resolve_status >= status;
        case ZigTypeIdUnion:
            return type_entry->data.unionation.resolve_status >= status;
        case ZigTypeIdEnum:
            return type_entry->data.enumeration.resolve_status >= status;
        case ZigTypeIdFnFrame:
            switch (status) {
                case ResolveStatusInvalid:
                    zig_unreachable();
                case ResolveStatusUnstarted:
                case ResolveStatusZeroBitsKnown:
                    return true;
                case ResolveStatusAlignmentKnown:
                case ResolveStatusSizeKnown:
                    return type_entry->data.frame.locals_struct != nullptr;
                case ResolveStatusLLVMFwdDecl:
                case ResolveStatusLLVMFull:
                    return type_entry->llvm_type != nullptr;
            }
        case ZigTypeIdOpaque:
            return status < ResolveStatusSizeKnown;
        case ZigTypeIdPointer:
            switch (status) {
                case ResolveStatusInvalid:
                    zig_unreachable();
                case ResolveStatusUnstarted:
                    return true;
                case ResolveStatusZeroBitsKnown:
                case ResolveStatusAlignmentKnown:
                case ResolveStatusSizeKnown:
                    return type_entry->abi_size != SIZE_MAX;
                case ResolveStatusLLVMFwdDecl:
                case ResolveStatusLLVMFull:
                    return type_entry->llvm_type != nullptr;
            }
        case ZigTypeIdMetaType:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdUnreachable:
        case ZigTypeIdInt:
        case ZigTypeIdFloat:
        case ZigTypeIdArray:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdOptional:
        case ZigTypeIdErrorUnion:
        case ZigTypeIdErrorSet:
        case ZigTypeIdFn:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
        case ZigTypeIdVector:
        case ZigTypeIdAnyFrame:
            return true;
    }
    zig_unreachable();
}

bool type_is_complete(ZigType *type_entry) {
    return type_is_resolved(type_entry, ResolveStatusSizeKnown);
}

uint64_t type_size(CodeGen *g, ZigType *type_entry) {
    assert(type_is_resolved(type_entry, ResolveStatusSizeKnown));
    return type_entry->abi_size;
}

uint64_t type_size_bits(CodeGen *g, ZigType *type_entry) {
    assert(type_is_resolved(type_entry, ResolveStatusSizeKnown));
    return type_entry->size_in_bits;
}

uint32_t get_abi_alignment(CodeGen *g, ZigType *type_entry) {
    assert(type_is_resolved(type_entry, ResolveStatusAlignmentKnown));
    return type_entry->abi_align;
}

static bool is_slice(ZigType *type) {
    return type->id == ZigTypeIdStruct && type->data.structure.is_slice;
}

ZigType *get_smallest_unsigned_int_type(CodeGen *g, uint64_t x) {
    return get_int_type(g, false, bits_needed_for_unsigned(x));
}

ZigType *get_any_frame_type(CodeGen *g, ZigType *result_type) {
    if (result_type != nullptr && result_type->any_frame_parent != nullptr) {
        return result_type->any_frame_parent;
    } else if (result_type == nullptr && g->builtin_types.entry_any_frame != nullptr) {
        return g->builtin_types.entry_any_frame;
    }

    ZigType *entry = new_type_table_entry(ZigTypeIdAnyFrame);
    entry->abi_size = g->builtin_types.entry_usize->abi_size;
    entry->size_in_bits = g->builtin_types.entry_usize->size_in_bits;
    entry->abi_align = g->builtin_types.entry_usize->abi_align;
    entry->data.any_frame.result_type = result_type;
    buf_init_from_str(&entry->name, "anyframe");
    if (result_type != nullptr) {
        buf_appendf(&entry->name, "->%s", buf_ptr(&result_type->name));
    }

    if (result_type != nullptr) {
        result_type->any_frame_parent = entry;
    } else if (result_type == nullptr) {
        g->builtin_types.entry_any_frame = entry;
    }
    return entry;
}

static const char *ptr_len_to_star_str(PtrLen ptr_len) {
    switch (ptr_len) {
        case PtrLenSingle:
            return "*";
        case PtrLenUnknown:
            return "[*]";
        case PtrLenC:
            return "[*c]";
    }
    zig_unreachable();
}

ZigType *get_fn_frame_type(CodeGen *g, ZigFn *fn) {
    if (fn->frame_type != nullptr) {
        return fn->frame_type;
    }

    ZigType *entry = new_type_table_entry(ZigTypeIdFnFrame);
    buf_resize(&entry->name, 0);
    buf_appendf(&entry->name, "@Frame(%s)", buf_ptr(&fn->symbol_name));

    entry->data.frame.fn = fn;

    // Async function frames are always non-zero bits because they always have a resume index.
    entry->abi_size = SIZE_MAX;
    entry->size_in_bits = SIZE_MAX;

    fn->frame_type = entry;
    return entry;
}

ZigType *get_pointer_to_type_extra(CodeGen *g, ZigType *child_type, bool is_const,
        bool is_volatile, PtrLen ptr_len, uint32_t byte_alignment,
        uint32_t bit_offset_in_host, uint32_t host_int_bytes, bool allow_zero)
{
    assert(ptr_len != PtrLenC || allow_zero);
    assert(!type_is_invalid(child_type));
    assert(ptr_len == PtrLenSingle || child_type->id != ZigTypeIdOpaque);

    if (byte_alignment != 0) {
        uint32_t abi_alignment = get_abi_alignment(g, child_type);
        if (byte_alignment == abi_alignment)
            byte_alignment = 0;
    }

    if (host_int_bytes != 0) {
        uint32_t child_type_bits = type_size_bits(g, child_type);
        if (host_int_bytes * 8 == child_type_bits) {
            assert(bit_offset_in_host == 0);
            host_int_bytes = 0;
        }
    }

    TypeId type_id = {};
    ZigType **parent_pointer = nullptr;
    if (host_int_bytes != 0 || is_volatile || byte_alignment != 0 || ptr_len != PtrLenSingle || allow_zero) {
        type_id.id = ZigTypeIdPointer;
        type_id.data.pointer.child_type = child_type;
        type_id.data.pointer.is_const = is_const;
        type_id.data.pointer.is_volatile = is_volatile;
        type_id.data.pointer.alignment = byte_alignment;
        type_id.data.pointer.bit_offset_in_host = bit_offset_in_host;
        type_id.data.pointer.host_int_bytes = host_int_bytes;
        type_id.data.pointer.ptr_len = ptr_len;
        type_id.data.pointer.allow_zero = allow_zero;

        auto existing_entry = g->type_table.maybe_get(type_id);
        if (existing_entry)
            return existing_entry->value;
    } else {
        assert(bit_offset_in_host == 0);
        parent_pointer = &child_type->pointer_parent[(is_const ? 1 : 0)];
        if (*parent_pointer) {
            assert((*parent_pointer)->data.pointer.explicit_alignment == 0);
            return *parent_pointer;
        }
    }

    ZigType *entry = new_type_table_entry(ZigTypeIdPointer);

    const char *star_str = ptr_len_to_star_str(ptr_len);
    const char *const_str = is_const ? "const " : "";
    const char *volatile_str = is_volatile ? "volatile " : "";
    const char *allow_zero_str;
    if (ptr_len == PtrLenC) {
        assert(allow_zero);
        allow_zero_str = "";
    } else {
        allow_zero_str = allow_zero ? "allowzero " : "";
    }
    buf_resize(&entry->name, 0);
    if (host_int_bytes == 0 && byte_alignment == 0) {
        buf_appendf(&entry->name, "%s%s%s%s%s",
                star_str, const_str, volatile_str, allow_zero_str, buf_ptr(&child_type->name));
    } else if (host_int_bytes == 0) {
        buf_appendf(&entry->name, "%salign(%" PRIu32 ") %s%s%s%s", star_str, byte_alignment,
                const_str, volatile_str, allow_zero_str, buf_ptr(&child_type->name));
    } else if (byte_alignment == 0) {
        buf_appendf(&entry->name, "%salign(:%" PRIu32 ":%" PRIu32 ") %s%s%s%s", star_str,
                bit_offset_in_host, host_int_bytes, const_str, volatile_str, allow_zero_str,
                buf_ptr(&child_type->name));
    } else {
        buf_appendf(&entry->name, "%salign(%" PRIu32 ":%" PRIu32 ":%" PRIu32 ") %s%s%s%s", star_str, byte_alignment,
                bit_offset_in_host, host_int_bytes, const_str, volatile_str, allow_zero_str,
                buf_ptr(&child_type->name));
    }

    if (type_is_resolved(child_type, ResolveStatusZeroBitsKnown)) {
        if (type_has_bits(child_type)) {
            entry->abi_size = g->builtin_types.entry_usize->abi_size;
            entry->size_in_bits = g->builtin_types.entry_usize->size_in_bits;
            entry->abi_align = g->builtin_types.entry_usize->abi_align;
        } else {
            assert(byte_alignment == 0);
            entry->abi_size = 0;
            entry->size_in_bits = 0;
            entry->abi_align = 0;
        }
    } else {
        entry->abi_size = SIZE_MAX;
        entry->size_in_bits = SIZE_MAX;
        entry->abi_align = UINT32_MAX;
    }

    entry->data.pointer.ptr_len = ptr_len;
    entry->data.pointer.child_type = child_type;
    entry->data.pointer.is_const = is_const;
    entry->data.pointer.is_volatile = is_volatile;
    entry->data.pointer.explicit_alignment = byte_alignment;
    entry->data.pointer.bit_offset_in_host = bit_offset_in_host;
    entry->data.pointer.host_int_bytes = host_int_bytes;
    entry->data.pointer.allow_zero = allow_zero;

    if (parent_pointer) {
        *parent_pointer = entry;
    } else {
        g->type_table.put(type_id, entry);
    }
    return entry;
}

ZigType *get_pointer_to_type(CodeGen *g, ZigType *child_type, bool is_const) {
    return get_pointer_to_type_extra(g, child_type, is_const, false, PtrLenSingle, 0, 0, 0, false);
}

ZigType *get_optional_type(CodeGen *g, ZigType *child_type) {
    if (child_type->optional_parent != nullptr) {
        return child_type->optional_parent;
    }

    assert(type_is_resolved(child_type, ResolveStatusSizeKnown));

    ZigType *entry = new_type_table_entry(ZigTypeIdOptional);

    buf_resize(&entry->name, 0);
    buf_appendf(&entry->name, "?%s", buf_ptr(&child_type->name));

    if (!type_has_bits(child_type)) {
        entry->size_in_bits = g->builtin_types.entry_bool->size_in_bits;
        entry->abi_size = g->builtin_types.entry_bool->abi_size;
        entry->abi_align = g->builtin_types.entry_bool->abi_align;
    } else if (type_is_nonnull_ptr(child_type) || child_type->id == ZigTypeIdErrorSet) {
        // This is an optimization but also is necessary for calling C
        // functions where all pointers are optional pointers.
        // Function types are technically pointers.
        entry->size_in_bits = child_type->size_in_bits;
        entry->abi_size = child_type->abi_size;
        entry->abi_align = child_type->abi_align;
    } else {
        // This value only matters if the type is legal in a packed struct, which is not
        // true for optional types which did not fit the above 2 categories (zero bit child type,
        // or nonnull ptr child type, or error set child type).
        entry->size_in_bits = child_type->size_in_bits + 1;

        // We're going to make a struct with the child type as the first field,
        // and a bool as the second. Since the child type's abi alignment is guaranteed
        // to be >= the bool's abi size (1 byte), the added size is exactly equal to the
        // child type's ABI alignment.
        assert(child_type->abi_align >= g->builtin_types.entry_bool->abi_size);
        entry->abi_align = child_type->abi_align;
        entry->abi_size = child_type->abi_size + child_type->abi_align;
    }

    entry->data.maybe.child_type = child_type;
    entry->data.maybe.resolve_status = ResolveStatusSizeKnown;

    child_type->optional_parent = entry;
    return entry;
}

static size_t align_forward(size_t addr, size_t alignment) {
    return (addr + alignment - 1) & ~(alignment - 1);
}

static size_t next_field_offset(size_t offset, size_t align_from_zero, size_t field_size, size_t next_field_align) {
    // Convert offset to a pretend address which has the specified alignment.
    size_t addr = offset + align_from_zero;
    // March the address forward to respect the field alignment.
    size_t aligned_addr = align_forward(addr + field_size, next_field_align);
    // Convert back from pretend address to offset.
    return aligned_addr - align_from_zero;
}

ZigType *get_error_union_type(CodeGen *g, ZigType *err_set_type, ZigType *payload_type) {
    assert(err_set_type->id == ZigTypeIdErrorSet);
    assert(!type_is_invalid(payload_type));

    TypeId type_id = {};
    type_id.id = ZigTypeIdErrorUnion;
    type_id.data.error_union.err_set_type = err_set_type;
    type_id.data.error_union.payload_type = payload_type;

    auto existing_entry = g->type_table.maybe_get(type_id);
    if (existing_entry) {
        return existing_entry->value;
    }

    ZigType *entry = new_type_table_entry(ZigTypeIdErrorUnion);
    assert(type_is_resolved(payload_type, ResolveStatusSizeKnown));

    buf_resize(&entry->name, 0);
    buf_appendf(&entry->name, "%s!%s", buf_ptr(&err_set_type->name), buf_ptr(&payload_type->name));

    entry->data.error_union.err_set_type = err_set_type;
    entry->data.error_union.payload_type = payload_type;

    if (!type_has_bits(payload_type)) {
        if (type_has_bits(err_set_type)) {
            entry->size_in_bits = err_set_type->size_in_bits;
            entry->abi_size = err_set_type->abi_size;
            entry->abi_align = err_set_type->abi_align;
        } else {
            entry->size_in_bits = 0;
            entry->abi_size = 0;
            entry->abi_align = 0;
        }
    } else if (!type_has_bits(err_set_type)) {
        entry->size_in_bits = payload_type->size_in_bits;
        entry->abi_size = payload_type->abi_size;
        entry->abi_align = payload_type->abi_align;
    } else {
        entry->abi_align = max(err_set_type->abi_align, payload_type->abi_align);
        size_t field_sizes[2];
        size_t field_aligns[2];
        field_sizes[err_union_err_index] = err_set_type->abi_size;
        field_aligns[err_union_err_index] = err_set_type->abi_align;
        field_sizes[err_union_payload_index] = payload_type->abi_size;
        field_aligns[err_union_payload_index] = payload_type->abi_align;
        size_t field2_offset = next_field_offset(0, entry->abi_align, field_sizes[0], field_aligns[1]);
        entry->abi_size = next_field_offset(field2_offset, entry->abi_align, field_sizes[1], entry->abi_align);
        entry->size_in_bits = entry->abi_size * 8;
        entry->data.error_union.pad_bytes = entry->abi_size - (field2_offset + field_sizes[1]);
    }

    g->type_table.put(type_id, entry);
    return entry;
}

ZigType *get_array_type(CodeGen *g, ZigType *child_type, uint64_t array_size) {
    TypeId type_id = {};
    type_id.id = ZigTypeIdArray;
    type_id.data.array.child_type = child_type;
    type_id.data.array.size = array_size;
    auto existing_entry = g->type_table.maybe_get(type_id);
    if (existing_entry) {
        return existing_entry->value;
    }

    assert(type_is_resolved(child_type, ResolveStatusSizeKnown));

    ZigType *entry = new_type_table_entry(ZigTypeIdArray);

    buf_resize(&entry->name, 0);
    buf_appendf(&entry->name, "[%" ZIG_PRI_u64 "]%s", array_size, buf_ptr(&child_type->name));

    entry->size_in_bits = child_type->size_in_bits * array_size;
    entry->abi_align = child_type->abi_align;
    entry->abi_size = child_type->abi_size * array_size;

    entry->data.array.child_type = child_type;
    entry->data.array.len = array_size;

    g->type_table.put(type_id, entry);
    return entry;
}

ZigType *get_slice_type(CodeGen *g, ZigType *ptr_type) {
    assert(ptr_type->id == ZigTypeIdPointer);
    assert(ptr_type->data.pointer.ptr_len == PtrLenUnknown);

    ZigType **parent_pointer = &ptr_type->data.pointer.slice_parent;
    if (*parent_pointer) {
        return *parent_pointer;
    }

    ZigType *entry = new_type_table_entry(ZigTypeIdStruct);

    // replace the & with [] to go from a ptr type name to a slice type name
    buf_resize(&entry->name, 0);
    size_t name_offset = (ptr_type->data.pointer.ptr_len == PtrLenSingle) ? 1 : 3;
    buf_appendf(&entry->name, "[]%s", buf_ptr(&ptr_type->name) + name_offset);

    unsigned element_count = 2;
    Buf *ptr_field_name = buf_create_from_str("ptr");
    Buf *len_field_name = buf_create_from_str("len");

    entry->data.structure.resolve_status = ResolveStatusSizeKnown;
    entry->data.structure.layout = ContainerLayoutAuto;
    entry->data.structure.is_slice = true;
    entry->data.structure.src_field_count = element_count;
    entry->data.structure.gen_field_count = element_count;
    entry->data.structure.fields = allocate<TypeStructField>(element_count);
    entry->data.structure.fields_by_name.init(element_count);
    entry->data.structure.fields[slice_ptr_index].name = ptr_field_name;
    entry->data.structure.fields[slice_ptr_index].type_entry = ptr_type;
    entry->data.structure.fields[slice_ptr_index].src_index = slice_ptr_index;
    entry->data.structure.fields[slice_ptr_index].gen_index = 0;
    entry->data.structure.fields[slice_len_index].name = len_field_name;
    entry->data.structure.fields[slice_len_index].type_entry = g->builtin_types.entry_usize;
    entry->data.structure.fields[slice_len_index].src_index = slice_len_index;
    entry->data.structure.fields[slice_len_index].gen_index = 1;

    entry->data.structure.fields_by_name.put(ptr_field_name, &entry->data.structure.fields[slice_ptr_index]);
    entry->data.structure.fields_by_name.put(len_field_name, &entry->data.structure.fields[slice_len_index]);

    switch (type_requires_comptime(g, ptr_type)) {
        case ReqCompTimeInvalid:
            zig_unreachable();
        case ReqCompTimeNo:
            break;
        case ReqCompTimeYes:
            entry->data.structure.requires_comptime = true;
    }

    if (!type_has_bits(ptr_type)) {
        entry->data.structure.gen_field_count = 1;
        entry->data.structure.fields[slice_ptr_index].gen_index = SIZE_MAX;
        entry->data.structure.fields[slice_len_index].gen_index = 0;
    }

    ZigType *child_type = ptr_type->data.pointer.child_type;
    if (ptr_type->data.pointer.is_const || ptr_type->data.pointer.is_volatile ||
        ptr_type->data.pointer.explicit_alignment != 0 || ptr_type->data.pointer.allow_zero)
    {
        ZigType *peer_ptr_type = get_pointer_to_type_extra(g, child_type, false, false,
                PtrLenUnknown, 0, 0, 0, false);
        ZigType *peer_slice_type = get_slice_type(g, peer_ptr_type);

        entry->size_in_bits = peer_slice_type->size_in_bits;
        entry->abi_size = peer_slice_type->abi_size;
        entry->abi_align = peer_slice_type->abi_align;

        *parent_pointer = entry;
        return entry;
    }

    if (type_has_bits(ptr_type)) {
        entry->size_in_bits = ptr_type->size_in_bits + g->builtin_types.entry_usize->size_in_bits;
        entry->abi_size = ptr_type->abi_size + g->builtin_types.entry_usize->abi_size;
        entry->abi_align = ptr_type->abi_align;
    } else {
        entry->size_in_bits = g->builtin_types.entry_usize->size_in_bits;
        entry->abi_size = g->builtin_types.entry_usize->abi_size;
        entry->abi_align = g->builtin_types.entry_usize->abi_align;
    }

    *parent_pointer = entry;
    return entry;
}

ZigType *get_opaque_type(CodeGen *g, Scope *scope, AstNode *source_node, const char *full_name, Buf *bare_name) {
    ZigType *entry = new_type_table_entry(ZigTypeIdOpaque);

    buf_init_from_str(&entry->name, full_name);

    ZigType *import = scope ? get_scope_import(scope) : nullptr;
    unsigned line = source_node ? (unsigned)(source_node->line + 1) : 0;

    entry->llvm_type = LLVMInt8Type();
    entry->llvm_di_type = ZigLLVMCreateDebugForwardDeclType(g->dbuilder,
        ZigLLVMTag_DW_structure_type(), full_name,
        import ? ZigLLVMFileToScope(import->data.structure.root_struct->di_file) : nullptr,
        import ? import->data.structure.root_struct->di_file : nullptr,
        line);
    entry->data.opaque.bare_name = bare_name;

    // The actual size is unknown, but the value must not be 0 because that
    // is how type_has_bits is determined.
    entry->abi_size = SIZE_MAX;
    entry->size_in_bits = SIZE_MAX;
    entry->abi_align = 1;

    return entry;
}

ZigType *get_bound_fn_type(CodeGen *g, ZigFn *fn_entry) {
    ZigType *fn_type = fn_entry->type_entry;
    assert(fn_type->id == ZigTypeIdFn);
    if (fn_type->data.fn.bound_fn_parent)
        return fn_type->data.fn.bound_fn_parent;

    ZigType *bound_fn_type = new_type_table_entry(ZigTypeIdBoundFn);
    bound_fn_type->data.bound_fn.fn_type = fn_type;

    buf_resize(&bound_fn_type->name, 0);
    buf_appendf(&bound_fn_type->name, "(bound %s)", buf_ptr(&fn_type->name));

    fn_type->data.fn.bound_fn_parent = bound_fn_type;
    return bound_fn_type;
}

const char *calling_convention_name(CallingConvention cc) {
    switch (cc) {
        case CallingConventionUnspecified: return "undefined";
        case CallingConventionC: return "ccc";
        case CallingConventionCold: return "coldcc";
        case CallingConventionNaked: return "nakedcc";
        case CallingConventionStdcall: return "stdcallcc";
        case CallingConventionAsync: return "async";
    }
    zig_unreachable();
}

static const char *calling_convention_fn_type_str(CallingConvention cc) {
    switch (cc) {
        case CallingConventionUnspecified: return "";
        case CallingConventionC: return "extern ";
        case CallingConventionCold: return "coldcc ";
        case CallingConventionNaked: return "nakedcc ";
        case CallingConventionStdcall: return "stdcallcc ";
        case CallingConventionAsync: return "async ";
    }
    zig_unreachable();
}

bool calling_convention_allows_zig_types(CallingConvention cc) {
    switch (cc) {
        case CallingConventionUnspecified:
        case CallingConventionAsync:
            return true;
        case CallingConventionC:
        case CallingConventionCold:
        case CallingConventionNaked:
        case CallingConventionStdcall:
            return false;
    }
    zig_unreachable();
}

ZigType *get_stack_trace_type(CodeGen *g) {
    if (g->stack_trace_type == nullptr) {
        ConstExprValue *stack_trace_type_val = get_builtin_value(g, "StackTrace");
        assert(stack_trace_type_val->type->id == ZigTypeIdMetaType);

        g->stack_trace_type = stack_trace_type_val->data.x_type;
        assertNoError(type_resolve(g, g->stack_trace_type, ResolveStatusZeroBitsKnown));
    }
    return g->stack_trace_type;
}

bool want_first_arg_sret(CodeGen *g, FnTypeId *fn_type_id) {
    if (fn_type_id->cc == CallingConventionUnspecified) {
        return handle_is_ptr(fn_type_id->return_type);
    }
    if (fn_type_id->cc != CallingConventionC) {
        return false;
    }
    if (type_is_c_abi_int(g, fn_type_id->return_type)) {
        return false;
    }
    if (g->zig_target->arch == ZigLLVM_x86 ||
        g->zig_target->arch == ZigLLVM_x86_64 ||
        target_is_arm(g->zig_target) ||
        target_is_riscv(g->zig_target))
    {
        X64CABIClass abi_class = type_c_abi_x86_64_class(g, fn_type_id->return_type);
        return abi_class == X64CABIClass_MEMORY || abi_class == X64CABIClass_MEMORY_nobyval;
    } else if (g->zig_target->arch == ZigLLVM_mipsel) {
        return false;
    }
    zig_panic("TODO implement C ABI for this architecture. See https://github.com/ziglang/zig/issues/1481");
}

ZigType *get_fn_type(CodeGen *g, FnTypeId *fn_type_id) {
    Error err;
    auto table_entry = g->fn_type_table.maybe_get(fn_type_id);
    if (table_entry) {
        return table_entry->value;
    }
    if (fn_type_id->return_type != nullptr) {
        if ((err = type_resolve(g, fn_type_id->return_type, ResolveStatusSizeKnown)))
            return g->builtin_types.entry_invalid;
        assert(fn_type_id->return_type->id != ZigTypeIdOpaque);
    } else {
        zig_panic("TODO implement inferred return types https://github.com/ziglang/zig/issues/447");
    }

    ZigType *fn_type = new_type_table_entry(ZigTypeIdFn);
    fn_type->data.fn.fn_type_id = *fn_type_id;

    // populate the name of the type
    buf_resize(&fn_type->name, 0);
    const char *cc_str = calling_convention_fn_type_str(fn_type->data.fn.fn_type_id.cc);
    buf_appendf(&fn_type->name, "%s", cc_str);
    buf_appendf(&fn_type->name, "fn(");
    for (size_t i = 0; i < fn_type_id->param_count; i += 1) {
        FnTypeParamInfo *param_info = &fn_type_id->param_info[i];

        ZigType *param_type = param_info->type;
        const char *comma = (i == 0) ? "" : ", ";
        const char *noalias_str = param_info->is_noalias ? "noalias " : "";
        buf_appendf(&fn_type->name, "%s%s%s", comma, noalias_str, buf_ptr(&param_type->name));
    }

    if (fn_type_id->is_var_args) {
        const char *comma = (fn_type_id->param_count == 0) ? "" : ", ";
        buf_appendf(&fn_type->name, "%s...", comma);
    }
    buf_appendf(&fn_type->name, ")");
    if (fn_type_id->alignment != 0) {
        buf_appendf(&fn_type->name, " align(%" PRIu32 ")", fn_type_id->alignment);
    }
    buf_appendf(&fn_type->name, " %s", buf_ptr(&fn_type_id->return_type->name));

    // The fn_type is a pointer; not to be confused with the raw function type.
    fn_type->size_in_bits = g->builtin_types.entry_usize->size_in_bits;
    fn_type->abi_size = g->builtin_types.entry_usize->abi_size;
    fn_type->abi_align = g->builtin_types.entry_usize->abi_align;

    g->fn_type_table.put(&fn_type->data.fn.fn_type_id, fn_type);

    return fn_type;
}

static ZigTypeId container_to_type(ContainerKind kind) {
    switch (kind) {
        case ContainerKindStruct:
            return ZigTypeIdStruct;
        case ContainerKindEnum:
            return ZigTypeIdEnum;
        case ContainerKindUnion:
            return ZigTypeIdUnion;
    }
    zig_unreachable();
}

// This is like get_partial_container_type except it's for the implicit root struct of files.
static ZigType *get_root_container_type(CodeGen *g, const char *full_name, Buf *bare_name,
        RootStruct *root_struct)
{
    ZigType *entry = new_type_table_entry(ZigTypeIdStruct);
    entry->data.structure.decls_scope = create_decls_scope(g, nullptr, nullptr, entry, entry, bare_name);
    entry->data.structure.root_struct = root_struct;
    entry->data.structure.layout = ContainerLayoutAuto;

    if (full_name[0] == '\0') {
        buf_init_from_str(&entry->name, "(root)");
    } else {
        buf_init_from_str(&entry->name, full_name);
    }

    return entry;
}

ZigType *get_partial_container_type(CodeGen *g, Scope *scope, ContainerKind kind,
        AstNode *decl_node, const char *full_name, Buf *bare_name, ContainerLayout layout)
{
    ZigTypeId type_id = container_to_type(kind);
    ZigType *entry = new_container_type_entry(g, type_id, decl_node, scope, bare_name);

    switch (kind) {
        case ContainerKindStruct:
            entry->data.structure.decl_node = decl_node;
            entry->data.structure.layout = layout;
            break;
        case ContainerKindEnum:
            entry->data.enumeration.decl_node = decl_node;
            entry->data.enumeration.layout = layout;
            break;
        case ContainerKindUnion:
            entry->data.unionation.decl_node = decl_node;
            entry->data.unionation.layout = layout;
            break;
    }

    buf_init_from_str(&entry->name, full_name);

    return entry;
}

ConstExprValue *analyze_const_value(CodeGen *g, Scope *scope, AstNode *node, ZigType *type_entry,
        Buf *type_name, UndefAllowed undef)
{
    size_t backward_branch_count = 0;
    size_t backward_branch_quota = default_backward_branch_quota;
    return ir_eval_const_value(g, scope, node, type_entry,
            &backward_branch_count, &backward_branch_quota,
            nullptr, nullptr, node, type_name, nullptr, nullptr, undef);
}

Error type_val_resolve_zero_bits(CodeGen *g, ConstExprValue *type_val, ZigType *parent_type,
        ConstExprValue *parent_type_val, bool *is_zero_bits)
{
    Error err;
    if (type_val->special != ConstValSpecialLazy) {
        assert(type_val->special == ConstValSpecialStatic);
        if ((type_val->data.x_type->id == ZigTypeIdStruct &&
            type_val->data.x_type->data.structure.resolve_loop_flag_zero_bits) ||
            (type_val->data.x_type->id == ZigTypeIdUnion &&
             type_val->data.x_type->data.unionation.resolve_loop_flag_zero_bits) ||
            type_val->data.x_type->id == ZigTypeIdPointer)
        {
            // Does a struct/union which contains a pointer field to itself have bits? Yes.
            *is_zero_bits = false;
            return ErrorNone;
        }
        if ((err = type_resolve(g, type_val->data.x_type, ResolveStatusZeroBitsKnown)))
            return err;
        *is_zero_bits = (type_val->data.x_type->abi_size == 0);
        return ErrorNone;
    }
    switch (type_val->data.x_lazy->id) {
        case LazyValueIdInvalid:
        case LazyValueIdAlignOf:
        case LazyValueIdSizeOf:
            zig_unreachable();
        case LazyValueIdPtrType: {
            LazyValuePtrType *lazy_ptr_type = reinterpret_cast<LazyValuePtrType *>(type_val->data.x_lazy);

            if (parent_type_val == &lazy_ptr_type->elem_type->value) {
                // Does a struct which contains a pointer field to itself have bits? Yes.
                *is_zero_bits = false;
                return ErrorNone;
            } else {
                if (parent_type_val == nullptr) {
                    parent_type_val = type_val;
                }
                return type_val_resolve_zero_bits(g, &lazy_ptr_type->elem_type->value, parent_type,
                        parent_type_val, is_zero_bits);
            }
        }
        case LazyValueIdOptType:
        case LazyValueIdSliceType:
        case LazyValueIdErrUnionType:
            *is_zero_bits = false;
            return ErrorNone;
        case LazyValueIdFnType: {
            LazyValueFnType *lazy_fn_type = reinterpret_cast<LazyValueFnType *>(type_val->data.x_lazy);
            *is_zero_bits = lazy_fn_type->is_generic;
            return ErrorNone;
        }
    }
    zig_unreachable();
}

Error type_val_resolve_is_opaque_type(CodeGen *g, ConstExprValue *type_val, bool *is_opaque_type) {
    if (type_val->special != ConstValSpecialLazy) {
        assert(type_val->special == ConstValSpecialStatic);
        *is_opaque_type = (type_val->data.x_type->id == ZigTypeIdOpaque);
        return ErrorNone;
    }
    switch (type_val->data.x_lazy->id) {
        case LazyValueIdInvalid:
        case LazyValueIdAlignOf:
        case LazyValueIdSizeOf:
            zig_unreachable();
        case LazyValueIdSliceType:
        case LazyValueIdPtrType:
        case LazyValueIdFnType:
        case LazyValueIdOptType:
        case LazyValueIdErrUnionType:
            *is_opaque_type = false;
            return ErrorNone;
    }
    zig_unreachable();
}

static ReqCompTime type_val_resolve_requires_comptime(CodeGen *g, ConstExprValue *type_val) {
    if (type_val->special != ConstValSpecialLazy) {
        return type_requires_comptime(g, type_val->data.x_type);
    }
    switch (type_val->data.x_lazy->id) {
        case LazyValueIdInvalid:
        case LazyValueIdAlignOf:
        case LazyValueIdSizeOf:
            zig_unreachable();
        case LazyValueIdSliceType: {
            LazyValueSliceType *lazy_slice_type = reinterpret_cast<LazyValueSliceType *>(type_val->data.x_lazy);
            return type_val_resolve_requires_comptime(g, &lazy_slice_type->elem_type->value);
        }
        case LazyValueIdPtrType: {
            LazyValuePtrType *lazy_ptr_type = reinterpret_cast<LazyValuePtrType *>(type_val->data.x_lazy);
            return type_val_resolve_requires_comptime(g, &lazy_ptr_type->elem_type->value);
        }
        case LazyValueIdOptType: {
            LazyValueOptType *lazy_opt_type = reinterpret_cast<LazyValueOptType *>(type_val->data.x_lazy);
            return type_val_resolve_requires_comptime(g, &lazy_opt_type->payload_type->value);
        }
        case LazyValueIdFnType: {
            LazyValueFnType *lazy_fn_type = reinterpret_cast<LazyValueFnType *>(type_val->data.x_lazy);
            if (lazy_fn_type->is_generic)
                return ReqCompTimeYes;
            switch (type_val_resolve_requires_comptime(g, &lazy_fn_type->return_type->value)) {
                case ReqCompTimeInvalid:
                    return ReqCompTimeInvalid;
                case ReqCompTimeYes:
                    return ReqCompTimeYes;
                case ReqCompTimeNo:
                    break;
            }
            size_t param_count = lazy_fn_type->proto_node->data.fn_proto.params.length;
            for (size_t i = 0; i < param_count; i += 1) {
                AstNode *param_node = lazy_fn_type->proto_node->data.fn_proto.params.at(i);
                bool param_is_var_args = param_node->data.param_decl.is_var_args;
                if (param_is_var_args) break;
                switch (type_val_resolve_requires_comptime(g, &lazy_fn_type->param_types[i]->value)) {
                    case ReqCompTimeInvalid:
                        return ReqCompTimeInvalid;
                    case ReqCompTimeYes:
                        return ReqCompTimeYes;
                    case ReqCompTimeNo:
                        break;
                }
            }
            return ReqCompTimeNo;
        }
        case LazyValueIdErrUnionType: {
            LazyValueErrUnionType *lazy_err_union_type =
                reinterpret_cast<LazyValueErrUnionType *>(type_val->data.x_lazy);
            return type_val_resolve_requires_comptime(g, &lazy_err_union_type->payload_type->value);
        }
    }
    zig_unreachable();
}

Error type_val_resolve_abi_size(CodeGen *g, AstNode *source_node, ConstExprValue *type_val,
        size_t *abi_size, size_t *size_in_bits)
{
    Error err;

start_over:
    if (type_val->special != ConstValSpecialLazy) {
        assert(type_val->special == ConstValSpecialStatic);
        ZigType *ty = type_val->data.x_type;
        if ((err = type_resolve(g, ty, ResolveStatusSizeKnown)))
            return err;
        *abi_size = ty->abi_size;
        *size_in_bits = ty->size_in_bits;
        return ErrorNone;
    }
    switch (type_val->data.x_lazy->id) {
        case LazyValueIdInvalid:
        case LazyValueIdAlignOf:
        case LazyValueIdSizeOf:
            zig_unreachable();
        case LazyValueIdSliceType: {
            LazyValueSliceType *lazy_slice_type = reinterpret_cast<LazyValueSliceType *>(type_val->data.x_lazy);
            bool is_zero_bits;
            if ((err = type_val_resolve_zero_bits(g, &lazy_slice_type->elem_type->value, nullptr,
                nullptr, &is_zero_bits)))
            {
                return err;
            }
            if (is_zero_bits) {
                *abi_size = g->builtin_types.entry_usize->abi_size;
                *size_in_bits = g->builtin_types.entry_usize->size_in_bits;
            } else {
                *abi_size = g->builtin_types.entry_usize->abi_size * 2;
                *size_in_bits = g->builtin_types.entry_usize->size_in_bits * 2;
            }
            return ErrorNone;
        }
        case LazyValueIdPtrType: {
            LazyValuePtrType *lazy_ptr_type = reinterpret_cast<LazyValuePtrType *>(type_val->data.x_lazy);
            bool is_zero_bits;
            if ((err = type_val_resolve_zero_bits(g, &lazy_ptr_type->elem_type->value, nullptr,
                nullptr, &is_zero_bits)))
            {
                return err;
            }
            if (is_zero_bits) {
                *abi_size = 0;
                *size_in_bits = 0;
            } else {
                *abi_size = g->builtin_types.entry_usize->abi_size;
                *size_in_bits = g->builtin_types.entry_usize->size_in_bits;
            }
            return ErrorNone;
        }
        case LazyValueIdFnType:
            *abi_size = g->builtin_types.entry_usize->abi_size;
            *size_in_bits = g->builtin_types.entry_usize->size_in_bits;
            return ErrorNone;
        case LazyValueIdOptType:
        case LazyValueIdErrUnionType:
            if ((err = ir_resolve_lazy(g, source_node, type_val)))
                return err;
            goto start_over;
    }
    zig_unreachable();
}

Error type_val_resolve_abi_align(CodeGen *g, ConstExprValue *type_val, uint32_t *abi_align) {
    Error err;
    if (type_val->special != ConstValSpecialLazy) {
        assert(type_val->special == ConstValSpecialStatic);
        ZigType *ty = type_val->data.x_type;
        if (ty->id == ZigTypeIdPointer) {
            *abi_align = g->builtin_types.entry_usize->abi_align;
            return ErrorNone;
        }
        if ((err = type_resolve(g, ty, ResolveStatusAlignmentKnown)))
            return err;
        *abi_align = ty->abi_align;
        return ErrorNone;
    }
    switch (type_val->data.x_lazy->id) {
        case LazyValueIdInvalid:
        case LazyValueIdAlignOf:
        case LazyValueIdSizeOf:
            zig_unreachable();
        case LazyValueIdSliceType:
        case LazyValueIdPtrType:
        case LazyValueIdFnType:
            *abi_align = g->builtin_types.entry_usize->abi_align;
            return ErrorNone;
        case LazyValueIdOptType: {
            LazyValueOptType *lazy_opt_type = reinterpret_cast<LazyValueOptType *>(type_val->data.x_lazy);
            return type_val_resolve_abi_align(g, &lazy_opt_type->payload_type->value, abi_align);
        }
        case LazyValueIdErrUnionType: {
            LazyValueErrUnionType *lazy_err_union_type =
                reinterpret_cast<LazyValueErrUnionType *>(type_val->data.x_lazy);
            uint32_t payload_abi_align;
            if ((err = type_val_resolve_abi_align(g, &lazy_err_union_type->payload_type->value,
                            &payload_abi_align)))
            {
                return err;
            }
            *abi_align = (payload_abi_align > g->err_tag_type->abi_align) ?
                payload_abi_align : g->err_tag_type->abi_align;
            return ErrorNone;
        }
    }
    zig_unreachable();
}

static OnePossibleValue type_val_resolve_has_one_possible_value(CodeGen *g, ConstExprValue *type_val) {
    if (type_val->special != ConstValSpecialLazy) {
        return type_has_one_possible_value(g, type_val->data.x_type);
    }
    switch (type_val->data.x_lazy->id) {
        case LazyValueIdInvalid:
        case LazyValueIdAlignOf:
        case LazyValueIdSizeOf:
            zig_unreachable();
        case LazyValueIdSliceType: // it has the len field
        case LazyValueIdOptType: // it has the optional bit
        case LazyValueIdFnType:
            return OnePossibleValueNo;
        case LazyValueIdPtrType: {
            Error err;
            bool zero_bits;
            if ((err = type_val_resolve_zero_bits(g, type_val, nullptr, nullptr, &zero_bits))) {
                return OnePossibleValueInvalid;
            }
            if (zero_bits) {
                return OnePossibleValueYes;
            } else {
                return OnePossibleValueNo;
            }
        }
        case LazyValueIdErrUnionType: {
            LazyValueErrUnionType *lazy_err_union_type =
                reinterpret_cast<LazyValueErrUnionType *>(type_val->data.x_lazy);
            switch (type_val_resolve_has_one_possible_value(g, &lazy_err_union_type->err_set_type->value)) {
                case OnePossibleValueInvalid:
                    return OnePossibleValueInvalid;
                case OnePossibleValueNo:
                    return OnePossibleValueNo;
                case OnePossibleValueYes:
                    return type_val_resolve_has_one_possible_value(g, &lazy_err_union_type->payload_type->value);
            }
        }
    }
    zig_unreachable();
}

ZigType *analyze_type_expr(CodeGen *g, Scope *scope, AstNode *node) {
    ConstExprValue *result = analyze_const_value(g, scope, node, g->builtin_types.entry_type,
            nullptr, UndefBad);
    if (type_is_invalid(result->type))
        return g->builtin_types.entry_invalid;
    src_assert(result->special == ConstValSpecialStatic, node);
    src_assert(result->data.x_type != nullptr, node);
    return result->data.x_type;
}

ZigType *get_generic_fn_type(CodeGen *g, FnTypeId *fn_type_id) {
    ZigType *fn_type = new_type_table_entry(ZigTypeIdFn);
    buf_resize(&fn_type->name, 0);
    const char *cc_str = calling_convention_fn_type_str(fn_type->data.fn.fn_type_id.cc);
    buf_appendf(&fn_type->name, "%s", cc_str);
    buf_appendf(&fn_type->name, "fn(");
    size_t i = 0;
    for (; i < fn_type_id->next_param_index; i += 1) {
        const char *comma_str = (i == 0) ? "" : ",";
        buf_appendf(&fn_type->name, "%s%s", comma_str,
            buf_ptr(&fn_type_id->param_info[i].type->name));
    }
    for (; i < fn_type_id->param_count; i += 1) {
        const char *comma_str = (i == 0) ? "" : ",";
        buf_appendf(&fn_type->name, "%svar", comma_str);
    }
    buf_appendf(&fn_type->name, ")var");

    fn_type->data.fn.fn_type_id = *fn_type_id;
    fn_type->data.fn.is_generic = true;
    fn_type->abi_size = 0;
    fn_type->size_in_bits = 0;
    fn_type->abi_align = 0;
    return fn_type;
}

void init_fn_type_id(FnTypeId *fn_type_id, AstNode *proto_node, size_t param_count_alloc) {
    assert(proto_node->type == NodeTypeFnProto);
    AstNodeFnProto *fn_proto = &proto_node->data.fn_proto;

    if (fn_proto->cc == CallingConventionUnspecified) {
        bool extern_abi = fn_proto->is_extern || fn_proto->is_export;
        fn_type_id->cc = extern_abi ? CallingConventionC : CallingConventionUnspecified;
    } else {
        fn_type_id->cc = fn_proto->cc;
    }

    fn_type_id->param_count = fn_proto->params.length;
    fn_type_id->param_info = allocate<FnTypeParamInfo>(param_count_alloc);
    fn_type_id->next_param_index = 0;
    fn_type_id->is_var_args = fn_proto->is_var_args;
}

static bool analyze_const_align(CodeGen *g, Scope *scope, AstNode *node, uint32_t *result) {
    ConstExprValue *align_result = analyze_const_value(g, scope, node, get_align_amt_type(g),
            nullptr, UndefBad);
    if (type_is_invalid(align_result->type))
        return false;

    uint32_t align_bytes = bigint_as_u32(&align_result->data.x_bigint);
    if (align_bytes == 0) {
        add_node_error(g, node, buf_sprintf("alignment must be >= 1"));
        return false;
    }
    if (!is_power_of_2(align_bytes)) {
        add_node_error(g, node, buf_sprintf("alignment value %" PRIu32 " is not a power of 2", align_bytes));
        return false;
    }

    *result = align_bytes;
    return true;
}

static bool analyze_const_string(CodeGen *g, Scope *scope, AstNode *node, Buf **out_buffer) {
    ZigType *ptr_type = get_pointer_to_type_extra(g, g->builtin_types.entry_u8, true, false,
            PtrLenUnknown, 0, 0, 0, false);
    ZigType *str_type = get_slice_type(g, ptr_type);
    ConstExprValue *result_val = analyze_const_value(g, scope, node, str_type, nullptr, UndefBad);
    if (type_is_invalid(result_val->type))
        return false;

    ConstExprValue *ptr_field = &result_val->data.x_struct.fields[slice_ptr_index];
    ConstExprValue *len_field = &result_val->data.x_struct.fields[slice_len_index];

    assert(ptr_field->data.x_ptr.special == ConstPtrSpecialBaseArray);
    ConstExprValue *array_val = ptr_field->data.x_ptr.data.base_array.array_val;
    if (array_val->data.x_array.special == ConstArraySpecialBuf) {
        *out_buffer = array_val->data.x_array.data.s_buf;
        return true;
    }
    expand_undef_array(g, array_val);
    size_t len = bigint_as_usize(&len_field->data.x_bigint);
    Buf *result = buf_alloc();
    buf_resize(result, len);
    for (size_t i = 0; i < len; i += 1) {
        size_t new_index = ptr_field->data.x_ptr.data.base_array.elem_index + i;
        ConstExprValue *char_val = &array_val->data.x_array.data.s_none.elements[new_index];
        if (char_val->special == ConstValSpecialUndef) {
            add_node_error(g, node, buf_sprintf("use of undefined value"));
            return false;
        }
        uint64_t big_c = bigint_as_u64(&char_val->data.x_bigint);
        assert(big_c <= UINT8_MAX);
        uint8_t c = (uint8_t)big_c;
        buf_ptr(result)[i] = c;
    }
    *out_buffer = result;
    return true;
}

static Error emit_error_unless_type_allowed_in_packed_container(CodeGen *g, ZigType *type_entry,
        AstNode *source_node, const char* container_name)
{
    Error err;
    switch (type_entry->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdMetaType:
        case ZigTypeIdUnreachable:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdErrorUnion:
        case ZigTypeIdErrorSet:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
        case ZigTypeIdOpaque:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            add_node_error(g, source_node,
                    buf_sprintf("type '%s' not allowed in packed %s; no guaranteed in-memory representation",
                        buf_ptr(&type_entry->name), container_name));
            return ErrorSemanticAnalyzeFail;
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdInt:
        case ZigTypeIdFloat:
        case ZigTypeIdPointer:
        case ZigTypeIdFn:
        case ZigTypeIdVector:
            return ErrorNone;
        case ZigTypeIdArray: {
            ZigType *elem_type = type_entry->data.array.child_type;
            if ((err = emit_error_unless_type_allowed_in_packed_container(g, elem_type, source_node, container_name)))
                return err;
            // TODO revisit this when doing https://github.com/ziglang/zig/issues/1512
            if (type_size(g, type_entry) * 8 == type_size_bits(g, type_entry))
                return ErrorNone;
            add_node_error(g, source_node,
                buf_sprintf("array of '%s' not allowed in packed %s due to padding bits",
                    buf_ptr(&elem_type->name), container_name));
            return ErrorSemanticAnalyzeFail;
        }
        case ZigTypeIdStruct:
            switch (type_entry->data.structure.layout) {
                case ContainerLayoutPacked:
                case ContainerLayoutExtern:
                    return ErrorNone;
                case ContainerLayoutAuto:
                    add_node_error(g, source_node,
                        buf_sprintf("non-packed, non-extern struct '%s' not allowed in packed %s; no guaranteed in-memory representation",
                            buf_ptr(&type_entry->name), container_name));
                    return ErrorSemanticAnalyzeFail;
            }
            zig_unreachable();
        case ZigTypeIdUnion:
            switch (type_entry->data.unionation.layout) {
                case ContainerLayoutPacked:
                case ContainerLayoutExtern:
                    return ErrorNone;
                case ContainerLayoutAuto:
                    add_node_error(g, source_node,
                        buf_sprintf("non-packed, non-extern union '%s' not allowed in packed %s; no guaranteed in-memory representation",
                            buf_ptr(&type_entry->name), container_name));
                    return ErrorSemanticAnalyzeFail;
            }
            zig_unreachable();
        case ZigTypeIdOptional:
            if (get_codegen_ptr_type(type_entry) != nullptr) {
                return ErrorNone;
            } else {
                add_node_error(g, source_node,
                    buf_sprintf("type '%s' not allowed in packed %s; no guaranteed in-memory representation",
                        buf_ptr(&type_entry->name), container_name));
                return ErrorSemanticAnalyzeFail;
            }
        case ZigTypeIdEnum: {
            AstNode *decl_node = type_entry->data.enumeration.decl_node;
            if (decl_node->data.container_decl.init_arg_expr != nullptr) {
                return ErrorNone;
            }
            ErrorMsg *msg = add_node_error(g, source_node,
                buf_sprintf("type '%s' not allowed in packed %s; no guaranteed in-memory representation",
                    buf_ptr(&type_entry->name), container_name));
            add_error_note(g, msg, decl_node,
                    buf_sprintf("enum declaration does not specify an integer tag type"));
            return ErrorSemanticAnalyzeFail;
        }
    }
    zig_unreachable();
}

static Error emit_error_unless_type_allowed_in_packed_struct(CodeGen *g, ZigType *type_entry,
    AstNode *source_node)
{
    return emit_error_unless_type_allowed_in_packed_container(g, type_entry, source_node, "struct");
}

static Error emit_error_unless_type_allowed_in_packed_union(CodeGen *g, ZigType *type_entry,
    AstNode *source_node)
{
    return emit_error_unless_type_allowed_in_packed_container(g, type_entry, source_node, "union");
}

Error type_allowed_in_extern(CodeGen *g, ZigType *type_entry, bool *result) {
    Error err;
    switch (type_entry->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdMetaType:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdErrorUnion:
        case ZigTypeIdErrorSet:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
        case ZigTypeIdVoid:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            *result = false;
            return ErrorNone;
        case ZigTypeIdOpaque:
        case ZigTypeIdUnreachable:
        case ZigTypeIdBool:
            *result = true;
            return ErrorNone;
        case ZigTypeIdInt:
            switch (type_entry->data.integral.bit_count) {
                case 8:
                case 16:
                case 32:
                case 64:
                case 128:
                    *result = true;
                    return ErrorNone;
                default:
                    *result = false;
                    return ErrorNone;
            }
        case ZigTypeIdVector:
            return type_allowed_in_extern(g, type_entry->data.vector.elem_type, result);
        case ZigTypeIdFloat:
            *result = true;
            return ErrorNone;
        case ZigTypeIdArray:
            return type_allowed_in_extern(g, type_entry->data.array.child_type, result);
        case ZigTypeIdFn:
            *result = type_entry->data.fn.fn_type_id.cc == CallingConventionC ||
                 type_entry->data.fn.fn_type_id.cc == CallingConventionStdcall;
            return ErrorNone;
        case ZigTypeIdPointer:
            if ((err = type_resolve(g, type_entry, ResolveStatusZeroBitsKnown)))
                return err;
            if (!type_has_bits(type_entry)) {
                *result = false;
                return ErrorNone;
            }
            *result = true;
            return ErrorNone;
        case ZigTypeIdStruct:
            *result = type_entry->data.structure.layout == ContainerLayoutExtern ||
                type_entry->data.structure.layout == ContainerLayoutPacked;
            return ErrorNone;
        case ZigTypeIdOptional: {
            ZigType *child_type = type_entry->data.maybe.child_type;
            if (child_type->id != ZigTypeIdPointer && child_type->id != ZigTypeIdFn) {
                *result = false;
                return ErrorNone;
            }
            if (!type_is_nonnull_ptr(child_type)) {
                *result = false;
                return ErrorNone;
            }
            return type_allowed_in_extern(g, child_type, result);
        }
        case ZigTypeIdEnum:
            *result = type_entry->data.enumeration.layout == ContainerLayoutExtern ||
                type_entry->data.enumeration.layout == ContainerLayoutPacked;
            return ErrorNone;
        case ZigTypeIdUnion:
            *result = type_entry->data.unionation.layout == ContainerLayoutExtern ||
                type_entry->data.unionation.layout == ContainerLayoutPacked;
            return ErrorNone;
    }
    zig_unreachable();
}

ZigType *get_auto_err_set_type(CodeGen *g, ZigFn *fn_entry) {
    ZigType *err_set_type = new_type_table_entry(ZigTypeIdErrorSet);
    buf_resize(&err_set_type->name, 0);
    buf_appendf(&err_set_type->name, "@typeOf(%s).ReturnType.ErrorSet", buf_ptr(&fn_entry->symbol_name));
    err_set_type->data.error_set.err_count = 0;
    err_set_type->data.error_set.errors = nullptr;
    err_set_type->data.error_set.infer_fn = fn_entry;
    err_set_type->data.error_set.incomplete = true;
    err_set_type->size_in_bits = g->builtin_types.entry_global_error_set->size_in_bits;
    err_set_type->abi_align = g->builtin_types.entry_global_error_set->abi_align;
    err_set_type->abi_size = g->builtin_types.entry_global_error_set->abi_size;

    return err_set_type;
}

static ZigType *analyze_fn_type(CodeGen *g, AstNode *proto_node, Scope *child_scope, ZigFn *fn_entry) {
    assert(proto_node->type == NodeTypeFnProto);
    AstNodeFnProto *fn_proto = &proto_node->data.fn_proto;
    Error err;

    FnTypeId fn_type_id = {0};
    init_fn_type_id(&fn_type_id, proto_node, proto_node->data.fn_proto.params.length);

    for (; fn_type_id.next_param_index < fn_type_id.param_count; fn_type_id.next_param_index += 1) {
        AstNode *param_node = fn_proto->params.at(fn_type_id.next_param_index);
        assert(param_node->type == NodeTypeParamDecl);

        bool param_is_comptime = param_node->data.param_decl.is_comptime;
        bool param_is_var_args = param_node->data.param_decl.is_var_args;

        if (param_is_comptime) {
            if (!calling_convention_allows_zig_types(fn_type_id.cc)) {
                add_node_error(g, param_node,
                        buf_sprintf("comptime parameter not allowed in function with calling convention '%s'",
                            calling_convention_name(fn_type_id.cc)));
                return g->builtin_types.entry_invalid;
            }
            if (param_node->data.param_decl.type != nullptr) {
                ZigType *type_entry = analyze_type_expr(g, child_scope, param_node->data.param_decl.type);
                if (type_is_invalid(type_entry)) {
                    return g->builtin_types.entry_invalid;
                }
                FnTypeParamInfo *param_info = &fn_type_id.param_info[fn_type_id.next_param_index];
                param_info->type = type_entry;
                param_info->is_noalias = param_node->data.param_decl.is_noalias;
                fn_type_id.next_param_index += 1;
            }

            return get_generic_fn_type(g, &fn_type_id);
        } else if (param_is_var_args) {
            if (fn_type_id.cc == CallingConventionC) {
                fn_type_id.param_count = fn_type_id.next_param_index;
                continue;
            } else if (calling_convention_allows_zig_types(fn_type_id.cc)) {
                return get_generic_fn_type(g, &fn_type_id);
            } else {
                add_node_error(g, param_node,
                        buf_sprintf("var args not allowed in function with calling convention '%s'",
                            calling_convention_name(fn_type_id.cc)));
                return g->builtin_types.entry_invalid;
            }
        } else if (param_node->data.param_decl.var_token != nullptr) {
            if (!calling_convention_allows_zig_types(fn_type_id.cc)) {
                add_node_error(g, param_node,
                        buf_sprintf("parameter of type 'var' not allowed in function with calling convention '%s'",
                            calling_convention_name(fn_type_id.cc)));
                return g->builtin_types.entry_invalid;
            }
            return get_generic_fn_type(g, &fn_type_id);
        }

        ZigType *type_entry = analyze_type_expr(g, child_scope, param_node->data.param_decl.type);
        if (type_is_invalid(type_entry)) {
            return g->builtin_types.entry_invalid;
        }
        if (!calling_convention_allows_zig_types(fn_type_id.cc)) {
            if ((err = type_resolve(g, type_entry, ResolveStatusZeroBitsKnown)))
                return g->builtin_types.entry_invalid;
            if (!type_has_bits(type_entry)) {
                add_node_error(g, param_node->data.param_decl.type,
                    buf_sprintf("parameter of type '%s' has 0 bits; not allowed in function with calling convention '%s'",
                        buf_ptr(&type_entry->name), calling_convention_name(fn_type_id.cc)));
                return g->builtin_types.entry_invalid;
            }
        }

        if (!calling_convention_allows_zig_types(fn_type_id.cc)) {
            bool ok_type;
            if ((err = type_allowed_in_extern(g, type_entry, &ok_type)))
                return g->builtin_types.entry_invalid;
            if (!ok_type) {
                add_node_error(g, param_node->data.param_decl.type,
                        buf_sprintf("parameter of type '%s' not allowed in function with calling convention '%s'",
                            buf_ptr(&type_entry->name),
                            calling_convention_name(fn_type_id.cc)));
                return g->builtin_types.entry_invalid;
            }
        }

        switch (type_entry->id) {
            case ZigTypeIdInvalid:
                zig_unreachable();
            case ZigTypeIdUnreachable:
            case ZigTypeIdUndefined:
            case ZigTypeIdNull:
            case ZigTypeIdArgTuple:
            case ZigTypeIdOpaque:
                add_node_error(g, param_node->data.param_decl.type,
                    buf_sprintf("parameter of type '%s' not allowed", buf_ptr(&type_entry->name)));
                return g->builtin_types.entry_invalid;
            case ZigTypeIdComptimeFloat:
            case ZigTypeIdComptimeInt:
            case ZigTypeIdEnumLiteral:
            case ZigTypeIdBoundFn:
            case ZigTypeIdMetaType:
            case ZigTypeIdVoid:
            case ZigTypeIdBool:
            case ZigTypeIdInt:
            case ZigTypeIdFloat:
            case ZigTypeIdPointer:
            case ZigTypeIdArray:
            case ZigTypeIdStruct:
            case ZigTypeIdOptional:
            case ZigTypeIdErrorUnion:
            case ZigTypeIdErrorSet:
            case ZigTypeIdEnum:
            case ZigTypeIdUnion:
            case ZigTypeIdFn:
            case ZigTypeIdVector:
            case ZigTypeIdFnFrame:
            case ZigTypeIdAnyFrame:
                switch (type_requires_comptime(g, type_entry)) {
                    case ReqCompTimeNo:
                        break;
                    case ReqCompTimeYes:
                        add_node_error(g, param_node->data.param_decl.type,
                            buf_sprintf("parameter of type '%s' must be declared comptime",
                            buf_ptr(&type_entry->name)));
                        return g->builtin_types.entry_invalid;
                    case ReqCompTimeInvalid:
                        return g->builtin_types.entry_invalid;
                }
                break;
        }
        FnTypeParamInfo *param_info = &fn_type_id.param_info[fn_type_id.next_param_index];
        param_info->type = type_entry;
        param_info->is_noalias = param_node->data.param_decl.is_noalias;
    }

    if (fn_proto->align_expr != nullptr) {
        if (!analyze_const_align(g, child_scope, fn_proto->align_expr, &fn_type_id.alignment)) {
            return g->builtin_types.entry_invalid;
        }
        fn_entry->align_bytes = fn_type_id.alignment;
    }

    if (fn_proto->return_var_token != nullptr) {
        if (!calling_convention_allows_zig_types(fn_type_id.cc)) {
            add_node_error(g, fn_proto->return_type,
                buf_sprintf("return type 'var' not allowed in function with calling convention '%s'",
                calling_convention_name(fn_type_id.cc)));
            return g->builtin_types.entry_invalid;
        }
        add_node_error(g, proto_node,
            buf_sprintf("TODO implement inferred return types https://github.com/ziglang/zig/issues/447"));
        return g->builtin_types.entry_invalid;
    }

    ZigType *specified_return_type = analyze_type_expr(g, child_scope, fn_proto->return_type);
    if (type_is_invalid(specified_return_type)) {
        fn_type_id.return_type = g->builtin_types.entry_invalid;
        return g->builtin_types.entry_invalid;
    }

    switch (specified_return_type->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();

        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdArgTuple:
            add_node_error(g, fn_proto->return_type,
                buf_sprintf("return type '%s' not allowed", buf_ptr(&specified_return_type->name)));
            return g->builtin_types.entry_invalid;

        case ZigTypeIdOpaque:
        {
            ErrorMsg* msg = add_node_error(g, fn_proto->return_type,
                buf_sprintf("opaque return type '%s' not allowed", buf_ptr(&specified_return_type->name)));
            Tld *tld = find_decl(g, &fn_entry->fndef_scope->base, &specified_return_type->name);
            if (tld != nullptr) {
                add_error_note(g, msg, tld->source_node, buf_sprintf("declared here"));
            }
            return g->builtin_types.entry_invalid;
        }

        default:
            break;
    }

    if (fn_proto->auto_err_set) {
        ZigType *inferred_err_set_type = get_auto_err_set_type(g, fn_entry);
        if ((err = type_resolve(g, specified_return_type, ResolveStatusSizeKnown)))
            return g->builtin_types.entry_invalid;
        fn_type_id.return_type = get_error_union_type(g, inferred_err_set_type, specified_return_type);
    } else {
        fn_type_id.return_type = specified_return_type;
    }

    if (!calling_convention_allows_zig_types(fn_type_id.cc) &&
        fn_type_id.return_type->id != ZigTypeIdVoid)
    {
        if ((err = type_resolve(g, fn_type_id.return_type, ResolveStatusSizeKnown)))
            return g->builtin_types.entry_invalid;
        bool ok_type;
        if ((err = type_allowed_in_extern(g, fn_type_id.return_type, &ok_type)))
            return g->builtin_types.entry_invalid;
        if (!ok_type) {
            add_node_error(g, fn_proto->return_type,
                    buf_sprintf("return type '%s' not allowed in function with calling convention '%s'",
                        buf_ptr(&fn_type_id.return_type->name),
                        calling_convention_name(fn_type_id.cc)));
            return g->builtin_types.entry_invalid;
        }
    }

    switch (fn_type_id.return_type->id) {
        case ZigTypeIdInvalid:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdArgTuple:
        case ZigTypeIdOpaque:
            zig_unreachable();

        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdBoundFn:
        case ZigTypeIdMetaType:
        case ZigTypeIdUnreachable:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdInt:
        case ZigTypeIdFloat:
        case ZigTypeIdPointer:
        case ZigTypeIdArray:
        case ZigTypeIdStruct:
        case ZigTypeIdOptional:
        case ZigTypeIdErrorUnion:
        case ZigTypeIdErrorSet:
        case ZigTypeIdEnum:
        case ZigTypeIdUnion:
        case ZigTypeIdFn:
        case ZigTypeIdVector:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            switch (type_requires_comptime(g, fn_type_id.return_type)) {
                case ReqCompTimeInvalid:
                    return g->builtin_types.entry_invalid;
                case ReqCompTimeYes:
                    return get_generic_fn_type(g, &fn_type_id);
                case ReqCompTimeNo:
                    break;
            }
            break;
    }

    return get_fn_type(g, &fn_type_id);
}

bool type_is_invalid(ZigType *type_entry) {
    switch (type_entry->id) {
        case ZigTypeIdInvalid:
            return true;
        case ZigTypeIdStruct:
            return type_entry->data.structure.resolve_status == ResolveStatusInvalid;
        case ZigTypeIdUnion:
            return type_entry->data.unionation.resolve_status == ResolveStatusInvalid;
        case ZigTypeIdEnum:
            return type_entry->data.enumeration.resolve_status == ResolveStatusInvalid;
        default:
            return false;
    }
    zig_unreachable();
}

struct SrcField {
    const char *name;
    ZigType *ty;
    unsigned align;
};

static ZigType *get_struct_type(CodeGen *g, const char *type_name, SrcField fields[], size_t field_count,
        unsigned min_abi_align)
{
    ZigType *struct_type = new_type_table_entry(ZigTypeIdStruct);

    buf_init_from_str(&struct_type->name, type_name);

    struct_type->data.structure.src_field_count = field_count;
    struct_type->data.structure.gen_field_count = 0;
    struct_type->data.structure.resolve_status = ResolveStatusSizeKnown;
    struct_type->data.structure.fields = allocate<TypeStructField>(field_count);
    struct_type->data.structure.fields_by_name.init(field_count);

    size_t abi_align = min_abi_align;
    for (size_t i = 0; i < field_count; i += 1) {
        TypeStructField *field = &struct_type->data.structure.fields[i];
        field->name = buf_create_from_str(fields[i].name);
        field->type_entry = fields[i].ty;
        field->src_index = i;
        field->align = fields[i].align;

        if (type_has_bits(field->type_entry)) {
            assert(type_is_resolved(field->type_entry, ResolveStatusSizeKnown));
            unsigned field_abi_align = max(field->align, field->type_entry->abi_align);
            if (field_abi_align > abi_align) {
                abi_align = field_abi_align;
            }
        }

        auto prev_entry = struct_type->data.structure.fields_by_name.put_unique(field->name, field);
        assert(prev_entry == nullptr);
    }

    size_t next_offset = 0;
    for (size_t i = 0; i < field_count; i += 1) {
        TypeStructField *field = &struct_type->data.structure.fields[i];
        if (!type_has_bits(field->type_entry))
            continue;

        field->offset = next_offset;

        // find the next non-zero-byte field for offset calculations
        size_t next_src_field_index = i + 1;
        for (; next_src_field_index < field_count; next_src_field_index += 1) {
            if (type_has_bits(struct_type->data.structure.fields[next_src_field_index].type_entry))
                break;
        }
        size_t next_abi_align;
        if (next_src_field_index == field_count) {
            next_abi_align = abi_align;
        } else {
            next_abi_align = max(fields[next_src_field_index].align,
                    struct_type->data.structure.fields[next_src_field_index].type_entry->abi_align);
        }
        next_offset = next_field_offset(next_offset, abi_align, field->type_entry->abi_size, next_abi_align);
    }

    struct_type->abi_align = abi_align;
    struct_type->abi_size = next_offset;
    struct_type->size_in_bits = next_offset * 8;

    return struct_type;
}

static size_t get_store_size_bytes(size_t size_in_bits) {
    return (size_in_bits + 7) / 8;
}

static size_t get_abi_align_bytes(size_t size_in_bits, size_t pointer_size_bytes) {
    size_t store_size_bytes = get_store_size_bytes(size_in_bits);
    if (store_size_bytes >= pointer_size_bytes)
        return pointer_size_bytes;
    return round_to_next_power_of_2(store_size_bytes);
}

static size_t get_abi_size_bytes(size_t size_in_bits, size_t pointer_size_bytes) {
    size_t store_size_bytes = get_store_size_bytes(size_in_bits);
    size_t abi_align = get_abi_align_bytes(size_in_bits, pointer_size_bytes);
    return align_forward(store_size_bytes, abi_align);
}

ZigType *resolve_struct_field_type(CodeGen *g, TypeStructField *struct_field) {
    Error err;
    if (struct_field->type_entry == nullptr) {
        if ((err = ir_resolve_lazy(g, struct_field->decl_node, struct_field->type_val))) {
            return nullptr;
        }
        struct_field->type_entry = struct_field->type_val->data.x_type;
    }
    return struct_field->type_entry;
}

static Error resolve_struct_type(CodeGen *g, ZigType *struct_type) {
    assert(struct_type->id == ZigTypeIdStruct);

    Error err;

    if (struct_type->data.structure.resolve_status == ResolveStatusInvalid)
        return ErrorSemanticAnalyzeFail;
    if (struct_type->data.structure.resolve_status >= ResolveStatusSizeKnown)
        return ErrorNone;

    if ((err = resolve_struct_alignment(g, struct_type)))
        return err;

    AstNode *decl_node = struct_type->data.structure.decl_node;

    if (struct_type->data.structure.resolve_loop_flag_other) {
        if (struct_type->data.structure.resolve_status != ResolveStatusInvalid) {
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            add_node_error(g, decl_node,
                buf_sprintf("struct '%s' depends on itself", buf_ptr(&struct_type->name)));
        }
        return ErrorSemanticAnalyzeFail;
    }

    assert(struct_type->data.structure.fields || struct_type->data.structure.src_field_count == 0);
    assert(decl_node->type == NodeTypeContainerDecl);

    size_t field_count = struct_type->data.structure.src_field_count;

    bool packed = (struct_type->data.structure.layout == ContainerLayoutPacked);
    struct_type->data.structure.resolve_loop_flag_other = true;

    uint32_t *host_int_bytes = packed ? allocate<uint32_t>(struct_type->data.structure.gen_field_count) : nullptr;

    size_t packed_bits_offset = 0;
    size_t next_offset = 0;
    size_t first_packed_bits_offset_misalign = SIZE_MAX;
    size_t gen_field_index = 0;
    size_t size_in_bits = 0;
    size_t abi_align = struct_type->abi_align;

    // Calculate offsets
    for (size_t i = 0; i < field_count; i += 1) {
        TypeStructField *field = &struct_type->data.structure.fields[i];
        if (field->gen_index == SIZE_MAX)
            continue;

        field->gen_index = gen_field_index;
        field->offset = next_offset;

        if (packed) {
            ZigType *field_type = resolve_struct_field_type(g, field);
            if (field_type == nullptr) {
                struct_type->data.structure.resolve_status = ResolveStatusInvalid;
                return err;
            }
            if ((err = type_resolve(g, field->type_entry, ResolveStatusSizeKnown))) {
                struct_type->data.structure.resolve_status = ResolveStatusInvalid;
                return err;
            }
            if ((err = emit_error_unless_type_allowed_in_packed_struct(g, field->type_entry, field->decl_node))) {
                struct_type->data.structure.resolve_status = ResolveStatusInvalid;
                return err;
            }

            size_t field_size_in_bits = type_size_bits(g, field_type);
            size_t next_packed_bits_offset = packed_bits_offset + field_size_in_bits;

            size_in_bits += field_size_in_bits;

            if (first_packed_bits_offset_misalign != SIZE_MAX) {
                // this field is not byte-aligned; it is part of the previous field with a bit offset
                field->bit_offset_in_host = packed_bits_offset - first_packed_bits_offset_misalign;

                size_t full_bit_count = next_packed_bits_offset - first_packed_bits_offset_misalign;
                size_t full_abi_size = get_abi_size_bytes(full_bit_count, g->pointer_size_bytes);
                if (full_abi_size * 8 == full_bit_count) {
                    // next field recovers ABI alignment
                    host_int_bytes[gen_field_index] = full_abi_size;
                    gen_field_index += 1;
                    // TODO: https://github.com/ziglang/zig/issues/1512
                    next_offset = next_field_offset(next_offset, abi_align, full_abi_size, 1);
                    size_in_bits = next_offset * 8;

                    first_packed_bits_offset_misalign = SIZE_MAX;
                }
            } else if (get_abi_size_bytes(field_type->size_in_bits, g->pointer_size_bytes) * 8 != field_size_in_bits) {
                first_packed_bits_offset_misalign = packed_bits_offset;
                field->bit_offset_in_host = 0;
            } else {
                // This is a byte-aligned field (both start and end) in a packed struct.
                host_int_bytes[gen_field_index] = field_type->size_in_bits / 8;
                field->bit_offset_in_host = 0;
                gen_field_index += 1;
                // TODO: https://github.com/ziglang/zig/issues/1512
                next_offset = next_field_offset(next_offset, abi_align, field_type->size_in_bits / 8, 1);
                size_in_bits = next_offset * 8;
            }
            packed_bits_offset = next_packed_bits_offset;
        } else {
            size_t field_abi_size;
            size_t field_size_in_bits;
            if ((err = type_val_resolve_abi_size(g, field->decl_node, field->type_val,
                &field_abi_size, &field_size_in_bits)))
            {
                struct_type->data.structure.resolve_status = ResolveStatusInvalid;
                return err;
            }

            gen_field_index += 1;
            size_t next_src_field_index = i + 1;
            for (; next_src_field_index < field_count; next_src_field_index += 1) {
                if (struct_type->data.structure.fields[next_src_field_index].gen_index != SIZE_MAX) {
                    break;
                }
            }
            size_t next_align = (next_src_field_index == field_count) ?
                abi_align : struct_type->data.structure.fields[next_src_field_index].align;
            next_offset = next_field_offset(next_offset, abi_align, field_abi_size, next_align);
            size_in_bits = next_offset * 8;
        }
    }
    if (first_packed_bits_offset_misalign != SIZE_MAX) {
        size_t full_bit_count = packed_bits_offset - first_packed_bits_offset_misalign;
        size_t full_abi_size = get_abi_size_bytes(full_bit_count, g->pointer_size_bytes);
        next_offset = next_field_offset(next_offset, abi_align, full_abi_size, abi_align);
        host_int_bytes[gen_field_index] = full_abi_size;
        gen_field_index += 1;
    }

    struct_type->abi_size = next_offset;
    struct_type->size_in_bits = size_in_bits;
    struct_type->data.structure.resolve_status = ResolveStatusSizeKnown;
    struct_type->data.structure.gen_field_count = (uint32_t)gen_field_index;
    struct_type->data.structure.resolve_loop_flag_other = false;
    struct_type->data.structure.host_int_bytes = host_int_bytes;


    // Resolve types for fields
    for (size_t i = 0; i < field_count; i += 1) {
        TypeStructField *field = &struct_type->data.structure.fields[i];
        ZigType *field_type = resolve_struct_field_type(g, field);
        if (field_type == nullptr) {
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            return err;
        }

        if ((err = type_resolve(g, field_type, ResolveStatusSizeKnown))) {
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            return err;
        }

        if (struct_type->data.structure.layout == ContainerLayoutExtern) {
            bool ok_type;
            if ((err = type_allowed_in_extern(g, field_type, &ok_type))) {
                struct_type->data.structure.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            }
            if (!ok_type) {
                add_node_error(g, field->decl_node,
                        buf_sprintf("extern structs cannot contain fields of type '%s'",
                            buf_ptr(&field_type->name)));
                struct_type->data.structure.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            }
        }
    }

    return ErrorNone;
}

static Error resolve_union_alignment(CodeGen *g, ZigType *union_type) {
    assert(union_type->id == ZigTypeIdUnion);

    Error err;

    if (union_type->data.unionation.resolve_status == ResolveStatusInvalid)
        return ErrorSemanticAnalyzeFail;
    if (union_type->data.unionation.resolve_status >= ResolveStatusAlignmentKnown)
        return ErrorNone;
    if ((err = resolve_union_zero_bits(g, union_type)))
        return err;
    if (union_type->data.unionation.resolve_status >= ResolveStatusAlignmentKnown)
        return ErrorNone;

    AstNode *decl_node = union_type->data.structure.decl_node;

    if (union_type->data.unionation.resolve_loop_flag_other) {
        if (union_type->data.unionation.resolve_status != ResolveStatusInvalid) {
            union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            add_node_error(g, decl_node,
                buf_sprintf("union '%s' depends on itself", buf_ptr(&union_type->name)));
        }
        return ErrorSemanticAnalyzeFail;
    }

    // set temporary flag
    union_type->data.unionation.resolve_loop_flag_other = true;

    TypeUnionField *most_aligned_union_member = nullptr;
    uint32_t field_count = union_type->data.unionation.src_field_count;
    bool packed = union_type->data.unionation.layout == ContainerLayoutPacked;

    for (uint32_t i = 0; i < field_count; i += 1) {
        TypeUnionField *field = &union_type->data.unionation.fields[i];
        if (field->gen_index == UINT32_MAX)
            continue;

        AstNode *align_expr = field->decl_node->data.struct_field.align_expr;
        if (align_expr != nullptr) {
            if (!analyze_const_align(g, &union_type->data.unionation.decls_scope->base, align_expr,
                        &field->align))
            {
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return err;
            }
            add_node_error(g, field->decl_node,
                buf_create_from_str("TODO implement field alignment syntax for unions. https://github.com/ziglang/zig/issues/3125"));
        } else if (packed) {
            field->align = 1;
        } else if (field->type_entry != nullptr) {
            if ((err = type_resolve(g, field->type_entry, ResolveStatusAlignmentKnown))) {
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return err;
            }
            field->align = field->type_entry->abi_align;
        } else {
            if ((err = type_val_resolve_abi_align(g, field->type_val, &field->align))) {
                if (g->trace_err != nullptr) {
                    g->trace_err = add_error_note(g, g->trace_err, field->decl_node,
                        buf_create_from_str("while checking this field"));
                }
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return err;
            }
            if (union_type->data.unionation.resolve_status == ResolveStatusInvalid)
                return ErrorSemanticAnalyzeFail;
        }

        if (most_aligned_union_member == nullptr || field->align > most_aligned_union_member->align) {
            most_aligned_union_member = field;
        }
    }

    // unset temporary flag
    union_type->data.unionation.resolve_loop_flag_other = false;
    union_type->data.unionation.resolve_status = ResolveStatusAlignmentKnown;
    union_type->data.unionation.most_aligned_union_member = most_aligned_union_member;

    ZigType *tag_type = union_type->data.unionation.tag_type;
    if (tag_type != nullptr && type_has_bits(tag_type)) {
        if ((err = type_resolve(g, tag_type, ResolveStatusAlignmentKnown))) {
            union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }
        if (most_aligned_union_member == nullptr) {
            union_type->abi_align = tag_type->abi_align;
            union_type->data.unionation.gen_tag_index = SIZE_MAX;
            union_type->data.unionation.gen_union_index = SIZE_MAX;
        } else if (tag_type->abi_align > most_aligned_union_member->align) {
            union_type->abi_align = tag_type->abi_align;
            union_type->data.unionation.gen_tag_index = 0;
            union_type->data.unionation.gen_union_index = 1;
        } else {
            union_type->abi_align = most_aligned_union_member->align;
            union_type->data.unionation.gen_union_index = 0;
            union_type->data.unionation.gen_tag_index = 1;
        }
    } else {
        assert(most_aligned_union_member != nullptr);
        union_type->abi_align = most_aligned_union_member->align;
        union_type->data.unionation.gen_union_index = SIZE_MAX;
        union_type->data.unionation.gen_tag_index = SIZE_MAX;
    }

    return ErrorNone;
}

ZigType *resolve_union_field_type(CodeGen *g, TypeUnionField *union_field) {
    Error err;
    if (union_field->type_entry == nullptr) {
        if ((err = ir_resolve_lazy(g, union_field->decl_node, union_field->type_val))) {
            return nullptr;
        }
        union_field->type_entry = union_field->type_val->data.x_type;
    }
    return union_field->type_entry;
}

static Error resolve_union_type(CodeGen *g, ZigType *union_type) {
    assert(union_type->id == ZigTypeIdUnion);

    Error err;

    if (union_type->data.unionation.resolve_status == ResolveStatusInvalid)
        return ErrorSemanticAnalyzeFail;
    if (union_type->data.unionation.resolve_status >= ResolveStatusSizeKnown)
        return ErrorNone;

    if ((err = resolve_union_alignment(g, union_type)))
        return err;

    AstNode *decl_node = union_type->data.unionation.decl_node;


    assert(decl_node->type == NodeTypeContainerDecl);

    uint32_t field_count = union_type->data.unionation.src_field_count;
    TypeUnionField *most_aligned_union_member = union_type->data.unionation.most_aligned_union_member;

    assert(union_type->data.unionation.fields);

    size_t union_abi_size = 0;
    size_t union_size_in_bits = 0;

    if (union_type->data.unionation.resolve_loop_flag_other) {
        if (union_type->data.unionation.resolve_status != ResolveStatusInvalid) {
            union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            add_node_error(g, decl_node,
                buf_sprintf("union '%s' depends on itself", buf_ptr(&union_type->name)));
        }
        return ErrorSemanticAnalyzeFail;
    }

    // set temporary flag
    union_type->data.unionation.resolve_loop_flag_other = true;

    const bool is_packed = union_type->data.unionation.layout == ContainerLayoutPacked;

    for (uint32_t i = 0; i < field_count; i += 1) {
        TypeUnionField *union_field = &union_type->data.unionation.fields[i];
        ZigType *field_type = resolve_union_field_type(g, union_field);
        if (field_type == nullptr) {
            union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }

        if ((err = type_resolve(g, field_type, ResolveStatusSizeKnown))) {
            union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }
        if (is_packed) {
            if ((err = emit_error_unless_type_allowed_in_packed_union(g, field_type, union_field->decl_node))) {
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return err;
            }
        }

        if (type_is_invalid(union_type))
            return ErrorSemanticAnalyzeFail;

        if (!type_has_bits(field_type))
            continue;

        union_abi_size = max(union_abi_size, field_type->abi_size);
        union_size_in_bits = max(union_size_in_bits, field_type->size_in_bits);
    }

    // The union itself for now has to be treated as being independently aligned.
    // See https://github.com/ziglang/zig/issues/2166.
    if (most_aligned_union_member != nullptr) {
        union_abi_size = align_forward(union_abi_size, most_aligned_union_member->align);
    }

    // unset temporary flag
    union_type->data.unionation.resolve_loop_flag_other = false;
    union_type->data.unionation.resolve_status = ResolveStatusSizeKnown;
    union_type->data.unionation.union_abi_size = union_abi_size;

    ZigType *tag_type = union_type->data.unionation.tag_type;
    if (tag_type != nullptr && type_has_bits(tag_type)) {
        if ((err = type_resolve(g, tag_type, ResolveStatusSizeKnown))) {
            union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }
        if (most_aligned_union_member == nullptr) {
            union_type->abi_size = tag_type->abi_size;
            union_type->size_in_bits = tag_type->size_in_bits;
        } else {
            size_t field_sizes[2];
            size_t field_aligns[2];
            field_sizes[union_type->data.unionation.gen_tag_index] = tag_type->abi_size;
            field_aligns[union_type->data.unionation.gen_tag_index] = tag_type->abi_align;
            field_sizes[union_type->data.unionation.gen_union_index] = union_abi_size;
            field_aligns[union_type->data.unionation.gen_union_index] = most_aligned_union_member->align;
            size_t field2_offset = next_field_offset(0, union_type->abi_align, field_sizes[0], field_aligns[1]);
            union_type->abi_size = next_field_offset(field2_offset, union_type->abi_align, field_sizes[1], union_type->abi_align);
            union_type->size_in_bits = union_type->abi_size * 8;
        }
    } else {
        union_type->abi_size = union_abi_size;
        union_type->size_in_bits = union_size_in_bits;
    }

    return ErrorNone;
}

static Error type_is_valid_extern_enum_tag(CodeGen *g, ZigType *ty, bool *result) {
    // Only integer types are allowed by the C ABI
    if(ty->id != ZigTypeIdInt) {
        *result = false;
        return ErrorNone;
    }

    // According to the ANSI C standard the enumeration type should be either a
    // signed char, a signed integer or an unsigned one. But GCC/Clang allow
    // other integral types as a compiler extension so let's accomodate them
    // aswell.
    return type_allowed_in_extern(g, ty, result);
}

static Error resolve_enum_zero_bits(CodeGen *g, ZigType *enum_type) {
    Error err;
    assert(enum_type->id == ZigTypeIdEnum);

    if (enum_type->data.enumeration.resolve_status == ResolveStatusInvalid)
        return ErrorSemanticAnalyzeFail;
    if (enum_type->data.enumeration.resolve_status >= ResolveStatusZeroBitsKnown)
        return ErrorNone;

    AstNode *decl_node = enum_type->data.enumeration.decl_node;
    assert(decl_node->type == NodeTypeContainerDecl);

    if (enum_type->data.enumeration.resolve_loop_flag) {
        if (enum_type->data.enumeration.resolve_status != ResolveStatusInvalid) {
            enum_type->data.enumeration.resolve_status = ResolveStatusInvalid;
            add_node_error(g, decl_node,
                buf_sprintf("enum '%s' depends on itself",
                    buf_ptr(&enum_type->name)));
        }
        return ErrorSemanticAnalyzeFail;
    }

    enum_type->data.enumeration.resolve_loop_flag = true;

    assert(!enum_type->data.enumeration.fields);
    uint32_t field_count = (uint32_t)decl_node->data.container_decl.fields.length;
    if (field_count == 0) {
        add_node_error(g, decl_node, buf_sprintf("enums must have 1 or more fields"));

        enum_type->data.enumeration.src_field_count = field_count;
        enum_type->data.enumeration.fields = nullptr;
        enum_type->data.enumeration.resolve_status = ResolveStatusInvalid;
        return ErrorSemanticAnalyzeFail;
    }

    enum_type->data.enumeration.src_field_count = field_count;
    enum_type->data.enumeration.fields = allocate<TypeEnumField>(field_count);
    enum_type->data.enumeration.fields_by_name.init(field_count);

    Scope *scope = &enum_type->data.enumeration.decls_scope->base;

    HashMap<BigInt, AstNode *, bigint_hash, bigint_eql> occupied_tag_values = {};
    occupied_tag_values.init(field_count);

    ZigType *tag_int_type;
    if (enum_type->data.enumeration.layout == ContainerLayoutExtern) {
        tag_int_type = get_c_int_type(g, CIntTypeInt);
    } else {
        tag_int_type = get_smallest_unsigned_int_type(g, field_count - 1);
    }

    enum_type->size_in_bits = tag_int_type->size_in_bits;
    enum_type->abi_size = tag_int_type->abi_size;
    enum_type->abi_align = tag_int_type->abi_align;

    if (decl_node->data.container_decl.init_arg_expr != nullptr) {
        ZigType *wanted_tag_int_type = analyze_type_expr(g, scope, decl_node->data.container_decl.init_arg_expr);
        if (type_is_invalid(wanted_tag_int_type)) {
            enum_type->data.enumeration.resolve_status = ResolveStatusInvalid;
        } else if (wanted_tag_int_type->id != ZigTypeIdInt &&
                   wanted_tag_int_type->id != ZigTypeIdComptimeInt) {
            enum_type->data.enumeration.resolve_status = ResolveStatusInvalid;
            add_node_error(g, decl_node->data.container_decl.init_arg_expr,
                buf_sprintf("expected integer, found '%s'", buf_ptr(&wanted_tag_int_type->name)));
        } else {
            if (enum_type->data.enumeration.layout == ContainerLayoutExtern) {
                bool ok_type;
                if ((err = type_is_valid_extern_enum_tag(g, wanted_tag_int_type, &ok_type))) {
                    enum_type->data.enumeration.resolve_status = ResolveStatusInvalid;
                    return err;
                }
                if (!ok_type) {
                    enum_type->data.enumeration.resolve_status = ResolveStatusInvalid;
                    ErrorMsg *msg = add_node_error(g, decl_node->data.container_decl.init_arg_expr,
                        buf_sprintf("'%s' is not a valid tag type for an extern enum",
                                    buf_ptr(&wanted_tag_int_type->name)));
                    add_error_note(g, msg, decl_node->data.container_decl.init_arg_expr,
                        buf_sprintf("any integral type of size 8, 16, 32, 64 or 128 bit is valid"));
                    return ErrorNone;
                }
            }
            tag_int_type = wanted_tag_int_type;
        }
    }

    enum_type->data.enumeration.tag_int_type = tag_int_type;
    enum_type->size_in_bits = tag_int_type->size_in_bits;
    enum_type->abi_size = tag_int_type->abi_size;
    enum_type->abi_align = tag_int_type->abi_align;

    BigInt bi_one;
    bigint_init_unsigned(&bi_one, 1);

    TypeEnumField *last_enum_field = nullptr;

    for (uint32_t field_i = 0; field_i < field_count; field_i += 1) {
        AstNode *field_node = decl_node->data.container_decl.fields.at(field_i);
        TypeEnumField *type_enum_field = &enum_type->data.enumeration.fields[field_i];
        type_enum_field->name = field_node->data.struct_field.name;
        type_enum_field->decl_index = field_i;
        type_enum_field->decl_node = field_node;

        if (field_node->data.struct_field.type != nullptr) {
            ErrorMsg *msg = add_node_error(g, field_node->data.struct_field.type,
                buf_sprintf("structs and unions, not enums, support field types"));
            add_error_note(g, msg, decl_node,
                    buf_sprintf("consider 'union(enum)' here"));
        } else if (field_node->data.struct_field.align_expr != nullptr) {
            ErrorMsg *msg = add_node_error(g, field_node->data.struct_field.align_expr,
                buf_sprintf("structs and unions, not enums, support field alignment"));
            add_error_note(g, msg, decl_node,
                    buf_sprintf("consider 'union(enum)' here"));
        }

        auto field_entry = enum_type->data.enumeration.fields_by_name.put_unique(type_enum_field->name, type_enum_field);
        if (field_entry != nullptr) {
            ErrorMsg *msg = add_node_error(g, field_node,
                buf_sprintf("duplicate enum field: '%s'", buf_ptr(type_enum_field->name)));
            add_error_note(g, msg, field_entry->value->decl_node, buf_sprintf("other field here"));
            enum_type->data.enumeration.resolve_status = ResolveStatusInvalid;
            continue;
        }

        AstNode *tag_value = field_node->data.struct_field.value;

        if (tag_value != nullptr) {
            // A user-specified value is available
            ConstExprValue *result = analyze_const_value(g, scope, tag_value, tag_int_type,
                    nullptr, UndefBad);
            if (type_is_invalid(result->type)) {
                enum_type->data.enumeration.resolve_status = ResolveStatusInvalid;
                continue;
            }

            assert(result->special != ConstValSpecialRuntime);
            assert(result->type->id == ZigTypeIdInt || result->type->id == ZigTypeIdComptimeInt);

            bigint_init_bigint(&type_enum_field->value, &result->data.x_bigint);
        } else {
            // No value was explicitly specified: allocate the last value + 1
            // or, if this is the first element, zero
            if (last_enum_field != nullptr) {
                bigint_add(&type_enum_field->value, &last_enum_field->value, &bi_one);
            } else {
                bigint_init_unsigned(&type_enum_field->value, 0);
            }

            // Make sure we can represent this number with tag_int_type
            if (!bigint_fits_in_bits(&type_enum_field->value,
                                     tag_int_type->size_in_bits,
                                     tag_int_type->data.integral.is_signed)) {
                enum_type->data.enumeration.resolve_status = ResolveStatusInvalid;

                Buf *val_buf = buf_alloc();
                bigint_append_buf(val_buf, &type_enum_field->value, 10);
                add_node_error(g, field_node,
                    buf_sprintf("enumeration value %s too large for type '%s'",
                        buf_ptr(val_buf), buf_ptr(&tag_int_type->name)));

                break;
            }
        }

        // Make sure the value is unique
        auto entry = occupied_tag_values.put_unique(type_enum_field->value, field_node);
        if (entry != nullptr) {
            enum_type->data.enumeration.resolve_status = ResolveStatusInvalid;

            Buf *val_buf = buf_alloc();
            bigint_append_buf(val_buf, &type_enum_field->value, 10);

            ErrorMsg *msg = add_node_error(g, field_node,
                    buf_sprintf("enum tag value %s already taken", buf_ptr(val_buf)));
            add_error_note(g, msg, entry->value,
                    buf_sprintf("other occurrence here"));
        }

        last_enum_field = type_enum_field;
    }

    if (enum_type->data.enumeration.resolve_status == ResolveStatusInvalid)
        return ErrorSemanticAnalyzeFail;

    enum_type->data.enumeration.resolve_loop_flag = false;
    enum_type->data.enumeration.resolve_status = ResolveStatusSizeKnown;

    occupied_tag_values.deinit();

    return ErrorNone;
}

static Error resolve_struct_zero_bits(CodeGen *g, ZigType *struct_type) {
    assert(struct_type->id == ZigTypeIdStruct);

    Error err;

    if (struct_type->data.structure.resolve_status == ResolveStatusInvalid)
        return ErrorSemanticAnalyzeFail;
    if (struct_type->data.structure.resolve_status >= ResolveStatusZeroBitsKnown)
        return ErrorNone;

    AstNode *decl_node = struct_type->data.structure.decl_node;
    assert(decl_node->type == NodeTypeContainerDecl);

    if (struct_type->data.structure.resolve_loop_flag_zero_bits) {
        if (struct_type->data.structure.resolve_status != ResolveStatusInvalid) {
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            add_node_error(g, decl_node,
                buf_sprintf("struct '%s' depends on itself",
                    buf_ptr(&struct_type->name)));
        }
        return ErrorSemanticAnalyzeFail;
    }

    struct_type->data.structure.resolve_loop_flag_zero_bits = true;

    assert(!struct_type->data.structure.fields);
    size_t field_count = decl_node->data.container_decl.fields.length;
    struct_type->data.structure.src_field_count = (uint32_t)field_count;
    struct_type->data.structure.fields = allocate<TypeStructField>(field_count);
    struct_type->data.structure.fields_by_name.init(field_count);

    Scope *scope = &struct_type->data.structure.decls_scope->base;

    size_t gen_field_index = 0;
    for (size_t i = 0; i < field_count; i += 1) {
        AstNode *field_node = decl_node->data.container_decl.fields.at(i);
        TypeStructField *type_struct_field = &struct_type->data.structure.fields[i];
        type_struct_field->name = field_node->data.struct_field.name;
        type_struct_field->decl_node = field_node;

        if (field_node->data.struct_field.type == nullptr) {
            add_node_error(g, field_node, buf_sprintf("struct field missing type"));
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }

        auto field_entry = struct_type->data.structure.fields_by_name.put_unique(type_struct_field->name, type_struct_field);
        if (field_entry != nullptr) {
            ErrorMsg *msg = add_node_error(g, field_node,
                buf_sprintf("duplicate struct field: '%s'", buf_ptr(type_struct_field->name)));
            add_error_note(g, msg, field_entry->value->decl_node, buf_sprintf("other field here"));
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }

        ConstExprValue *field_type_val = analyze_const_value(g, scope,
                field_node->data.struct_field.type, g->builtin_types.entry_type, nullptr, LazyOkNoUndef);
        if (type_is_invalid(field_type_val->type)) {
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }
        assert(field_type_val->special != ConstValSpecialRuntime);
        type_struct_field->type_val = field_type_val;
        if (struct_type->data.structure.resolve_status == ResolveStatusInvalid)
            return ErrorSemanticAnalyzeFail;

        bool field_is_opaque_type;
        if ((err = type_val_resolve_is_opaque_type(g, field_type_val, &field_is_opaque_type))) {
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }
        if (field_is_opaque_type) {
            add_node_error(g, field_node->data.struct_field.type,
                buf_sprintf("opaque types have unknown size and therefore cannot be directly embedded in structs"));
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }

        type_struct_field->src_index = i;
        type_struct_field->gen_index = SIZE_MAX;

        switch (type_val_resolve_requires_comptime(g, field_type_val)) {
            case ReqCompTimeYes:
                struct_type->data.structure.requires_comptime = true;
                break;
            case ReqCompTimeInvalid:
                if (g->trace_err != nullptr) {
                    g->trace_err = add_error_note(g, g->trace_err, field_node,
                        buf_create_from_str("while checking this field"));
                }
                struct_type->data.structure.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            case ReqCompTimeNo:
                break;
        }

        bool field_is_zero_bits;
        if ((err = type_val_resolve_zero_bits(g, field_type_val, struct_type, nullptr, &field_is_zero_bits))) {
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }
        if (field_is_zero_bits)
            continue;

        type_struct_field->gen_index = gen_field_index;
        gen_field_index += 1;
    }

    struct_type->data.structure.resolve_loop_flag_zero_bits = false;
    struct_type->data.structure.gen_field_count = (uint32_t)gen_field_index;
    if (gen_field_index != 0) {
        struct_type->abi_size = SIZE_MAX;
        struct_type->size_in_bits = SIZE_MAX;
    }

    if (struct_type->data.structure.resolve_status == ResolveStatusInvalid)
        return ErrorSemanticAnalyzeFail;

    struct_type->data.structure.resolve_status = ResolveStatusZeroBitsKnown;
    return ErrorNone;
}

static Error resolve_struct_alignment(CodeGen *g, ZigType *struct_type) {
    assert(struct_type->id == ZigTypeIdStruct);

    Error err;

    if (struct_type->data.structure.resolve_status == ResolveStatusInvalid)
        return ErrorSemanticAnalyzeFail;
    if (struct_type->data.structure.resolve_status >= ResolveStatusAlignmentKnown)
        return ErrorNone;
    if ((err = resolve_struct_zero_bits(g, struct_type)))
        return err;
    if (struct_type->data.structure.resolve_status >= ResolveStatusAlignmentKnown)
        return ErrorNone;

    AstNode *decl_node = struct_type->data.structure.decl_node;

    if (struct_type->data.structure.resolve_loop_flag_other) {
        if (struct_type->data.structure.resolve_status != ResolveStatusInvalid) {
            struct_type->data.structure.resolve_status = ResolveStatusInvalid;
            add_node_error(g, decl_node,
                buf_sprintf("struct '%s' depends on itself", buf_ptr(&struct_type->name)));
        }
        return ErrorSemanticAnalyzeFail;
    }

    struct_type->data.structure.resolve_loop_flag_other = true;
    assert(decl_node->type == NodeTypeContainerDecl);

    size_t field_count = struct_type->data.structure.src_field_count;
    bool packed = struct_type->data.structure.layout == ContainerLayoutPacked;

    for (size_t i = 0; i < field_count; i += 1) {
        TypeStructField *field = &struct_type->data.structure.fields[i];
        if (field->gen_index == SIZE_MAX)
            continue;

        AstNode *align_expr = field->decl_node->data.struct_field.align_expr;
        if (align_expr != nullptr) {
            if (!analyze_const_align(g, &struct_type->data.structure.decls_scope->base, align_expr,
                        &field->align))
            {
                struct_type->data.structure.resolve_status = ResolveStatusInvalid;
                return err;
            }
        } else if (packed) {
            field->align = 1;
        } else {
            if ((err = type_val_resolve_abi_align(g, field->type_val, &field->align))) {
                if (g->trace_err != nullptr) {
                    g->trace_err = add_error_note(g, g->trace_err, field->decl_node,
                        buf_create_from_str("while checking this field"));
                }
                struct_type->data.structure.resolve_status = ResolveStatusInvalid;
                return err;
            }
            if (struct_type->data.structure.resolve_status == ResolveStatusInvalid)
                return ErrorSemanticAnalyzeFail;
        }

        if (field->align > struct_type->abi_align) {
            struct_type->abi_align = field->align;
        }
    }

    if (!type_has_bits(struct_type)) {
        assert(struct_type->abi_align == 0);
    }

    struct_type->data.structure.resolve_loop_flag_other = false;

    if (struct_type->data.structure.resolve_status == ResolveStatusInvalid) {
        return ErrorSemanticAnalyzeFail;
    }

    struct_type->data.structure.resolve_status = ResolveStatusAlignmentKnown;
    return ErrorNone;
}

static Error resolve_union_zero_bits(CodeGen *g, ZigType *union_type) {
    assert(union_type->id == ZigTypeIdUnion);

    Error err;

    if (union_type->data.unionation.resolve_status == ResolveStatusInvalid)
        return ErrorSemanticAnalyzeFail;

    if (union_type->data.unionation.resolve_status >= ResolveStatusZeroBitsKnown)
        return ErrorNone;

    AstNode *decl_node = union_type->data.unionation.decl_node;
    assert(decl_node->type == NodeTypeContainerDecl);

    if (union_type->data.unionation.resolve_loop_flag_zero_bits) {
        if (union_type->data.unionation.resolve_status != ResolveStatusInvalid) {
            union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            add_node_error(g, decl_node,
                buf_sprintf("union '%s' depends on itself",
                    buf_ptr(&union_type->name)));
        }
        return ErrorSemanticAnalyzeFail;
    }

    union_type->data.unionation.resolve_loop_flag_zero_bits = true;

    assert(union_type->data.unionation.fields == nullptr);
    uint32_t field_count = (uint32_t)decl_node->data.container_decl.fields.length;
    if (field_count == 0) {
        add_node_error(g, decl_node, buf_sprintf("unions must have 1 or more fields"));
        union_type->data.unionation.src_field_count = field_count;
        union_type->data.unionation.resolve_status = ResolveStatusInvalid;
        return ErrorSemanticAnalyzeFail;
    }
    union_type->data.unionation.src_field_count = field_count;
    union_type->data.unionation.fields = allocate<TypeUnionField>(field_count);
    union_type->data.unionation.fields_by_name.init(field_count);

    Scope *scope = &union_type->data.unionation.decls_scope->base;

    HashMap<BigInt, AstNode *, bigint_hash, bigint_eql> occupied_tag_values = {};

    AstNode *enum_type_node = decl_node->data.container_decl.init_arg_expr;
    union_type->data.unionation.have_explicit_tag_type = decl_node->data.container_decl.auto_enum ||
        enum_type_node != nullptr;
    bool auto_layout = (union_type->data.unionation.layout == ContainerLayoutAuto);
    bool want_safety = (field_count >= 2) && (auto_layout || enum_type_node != nullptr) && !(g->build_mode == BuildModeFastRelease || g->build_mode == BuildModeSmallRelease);
    ZigType *tag_type;
    bool create_enum_type = decl_node->data.container_decl.auto_enum || (enum_type_node == nullptr && want_safety);
    bool *covered_enum_fields;
    ZigLLVMDIEnumerator **di_enumerators;
    if (create_enum_type) {
        occupied_tag_values.init(field_count);

        di_enumerators = allocate<ZigLLVMDIEnumerator*>(field_count);

        ZigType *tag_int_type;
        if (enum_type_node != nullptr) {
            tag_int_type = analyze_type_expr(g, scope, enum_type_node);
            if (type_is_invalid(tag_int_type)) {
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            }
            if (tag_int_type->id != ZigTypeIdInt && tag_int_type->id != ZigTypeIdComptimeInt) {
                add_node_error(g, enum_type_node,
                    buf_sprintf("expected integer tag type, found '%s'", buf_ptr(&tag_int_type->name)));
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            }
        } else {
            tag_int_type = get_smallest_unsigned_int_type(g, field_count - 1);
        }

        tag_type = new_type_table_entry(ZigTypeIdEnum);
        buf_resize(&tag_type->name, 0);
        buf_appendf(&tag_type->name, "@TagType(%s)", buf_ptr(&union_type->name));
        tag_type->llvm_type = tag_int_type->llvm_type;
        tag_type->llvm_di_type = tag_int_type->llvm_di_type;
        tag_type->abi_size = tag_int_type->abi_size;
        tag_type->abi_align = tag_int_type->abi_align;
        tag_type->size_in_bits = tag_int_type->size_in_bits;

        tag_type->data.enumeration.tag_int_type = tag_int_type;
        tag_type->data.enumeration.resolve_status = ResolveStatusSizeKnown;
        tag_type->data.enumeration.decl_node = decl_node;
        tag_type->data.enumeration.layout = ContainerLayoutAuto;
        tag_type->data.enumeration.src_field_count = field_count;
        tag_type->data.enumeration.fields = allocate<TypeEnumField>(field_count);
        tag_type->data.enumeration.fields_by_name.init(field_count);
        tag_type->data.enumeration.decls_scope = union_type->data.unionation.decls_scope;
    } else if (enum_type_node != nullptr) {
        ZigType *enum_type = analyze_type_expr(g, scope, enum_type_node);
        if (type_is_invalid(enum_type)) {
            union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }
        if (enum_type->id != ZigTypeIdEnum) {
            union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            add_node_error(g, enum_type_node,
                buf_sprintf("expected enum tag type, found '%s'", buf_ptr(&enum_type->name)));
            return ErrorSemanticAnalyzeFail;
        }
        if ((err = type_resolve(g, enum_type, ResolveStatusAlignmentKnown))) {
            assert(g->errors.length != 0);
            return err;
        }
        tag_type = enum_type;
        covered_enum_fields = allocate<bool>(enum_type->data.enumeration.src_field_count);
    } else {
        tag_type = nullptr;
    }
    union_type->data.unionation.tag_type = tag_type;

    uint32_t gen_field_index = 0;
    for (uint32_t i = 0; i < field_count; i += 1) {
        AstNode *field_node = decl_node->data.container_decl.fields.at(i);
        Buf *field_name = field_node->data.struct_field.name;
        TypeUnionField *union_field = &union_type->data.unionation.fields[i];
        union_field->name = field_node->data.struct_field.name;
        union_field->decl_node = field_node;
        union_field->gen_index = UINT32_MAX;

        auto field_entry = union_type->data.unionation.fields_by_name.put_unique(union_field->name, union_field);
        if (field_entry != nullptr) {
            ErrorMsg *msg = add_node_error(g, field_node,
                buf_sprintf("duplicate union field: '%s'", buf_ptr(union_field->name)));
            add_error_note(g, msg, field_entry->value->decl_node, buf_sprintf("other field here"));
            union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            return ErrorSemanticAnalyzeFail;
        }

        bool field_is_zero_bits;
        if (field_node->data.struct_field.type == nullptr) {
            if (decl_node->data.container_decl.auto_enum ||
                decl_node->data.container_decl.init_arg_expr != nullptr)
            {
                union_field->type_entry = g->builtin_types.entry_void;
                field_is_zero_bits = true;
            } else {
                add_node_error(g, field_node, buf_sprintf("union field missing type"));
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            }
        } else {
            ConstExprValue *field_type_val = analyze_const_value(g, scope,
                    field_node->data.struct_field.type, g->builtin_types.entry_type, nullptr, LazyOkNoUndef);
            if (type_is_invalid(field_type_val->type)) {
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            }
            assert(field_type_val->special != ConstValSpecialRuntime);
            union_field->type_val = field_type_val;
            if (union_type->data.unionation.resolve_status == ResolveStatusInvalid)
                return ErrorSemanticAnalyzeFail;

            bool field_is_opaque_type;
            if ((err = type_val_resolve_is_opaque_type(g, field_type_val, &field_is_opaque_type))) {
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            }
            if (field_is_opaque_type) {
                add_node_error(g, field_node->data.struct_field.type,
                    buf_create_from_str(
                        "opaque types have unknown size and therefore cannot be directly embedded in unions"));
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            }

            switch (type_val_resolve_requires_comptime(g, field_type_val)) {
                case ReqCompTimeInvalid:
                    if (g->trace_err != nullptr) {
                        g->trace_err = add_error_note(g, g->trace_err, field_node,
                            buf_create_from_str("while checking this field"));
                    }
                    union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                    return ErrorSemanticAnalyzeFail;
                case ReqCompTimeYes:
                    union_type->data.unionation.requires_comptime = true;
                    break;
                case ReqCompTimeNo:
                    break;
            }

            if ((err = type_val_resolve_zero_bits(g, field_type_val, union_type, nullptr, &field_is_zero_bits))) {
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            }
        }

        if (field_node->data.struct_field.value != nullptr && !decl_node->data.container_decl.auto_enum) {
            ErrorMsg *msg = add_node_error(g, field_node->data.struct_field.value,
                    buf_create_from_str("untagged union field assignment"));
            add_error_note(g, msg, decl_node, buf_create_from_str("consider 'union(enum)' here"));
        }

        if (create_enum_type) {
            di_enumerators[i] = ZigLLVMCreateDebugEnumerator(g->dbuilder, buf_ptr(field_name), i);
            union_field->enum_field = &tag_type->data.enumeration.fields[i];
            union_field->enum_field->name = field_name;
            union_field->enum_field->decl_index = i;
            union_field->enum_field->decl_node = field_node;

            auto prev_entry = tag_type->data.enumeration.fields_by_name.put_unique(union_field->enum_field->name, union_field->enum_field);
            assert(prev_entry == nullptr); // caught by union de-duplicator above

            AstNode *tag_value = field_node->data.struct_field.value;
            // In this first pass we resolve explicit tag values.
            // In a second pass we will fill in the unspecified ones.
            if (tag_value != nullptr) {
                ZigType *tag_int_type = tag_type->data.enumeration.tag_int_type;
                ConstExprValue *result = analyze_const_value(g, scope, tag_value, tag_int_type,
                        nullptr, UndefBad);
                if (type_is_invalid(result->type)) {
                    union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                    return ErrorSemanticAnalyzeFail;
                }
                assert(result->special != ConstValSpecialRuntime);
                assert(result->type->id == ZigTypeIdInt);
                auto entry = occupied_tag_values.put_unique(result->data.x_bigint, tag_value);
                if (entry == nullptr) {
                    bigint_init_bigint(&union_field->enum_field->value, &result->data.x_bigint);
                } else {
                    Buf *val_buf = buf_alloc();
                    bigint_append_buf(val_buf, &result->data.x_bigint, 10);

                    ErrorMsg *msg = add_node_error(g, tag_value,
                            buf_sprintf("enum tag value %s already taken", buf_ptr(val_buf)));
                    add_error_note(g, msg, entry->value,
                            buf_sprintf("other occurrence here"));
                    union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                    return ErrorSemanticAnalyzeFail;
                }
            }
        } else if (enum_type_node != nullptr) {
            union_field->enum_field = find_enum_type_field(tag_type, field_name);
            if (union_field->enum_field == nullptr) {
                ErrorMsg *msg = add_node_error(g, field_node,
                    buf_sprintf("enum field not found: '%s'", buf_ptr(field_name)));
                add_error_note(g, msg, tag_type->data.enumeration.decl_node,
                        buf_sprintf("enum declared here"));
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
                return ErrorSemanticAnalyzeFail;
            }
            covered_enum_fields[union_field->enum_field->decl_index] = true;
        } else {
            union_field->enum_field = allocate<TypeEnumField>(1);
            union_field->enum_field->name = field_name;
            union_field->enum_field->decl_index = i;
            bigint_init_unsigned(&union_field->enum_field->value, i);
        }
        assert(union_field->enum_field != nullptr);

        if (field_is_zero_bits)
            continue;

        union_field->gen_index = gen_field_index;
        gen_field_index += 1;
    }

    bool src_have_tag = decl_node->data.container_decl.auto_enum ||
        decl_node->data.container_decl.init_arg_expr != nullptr;

    if (src_have_tag && union_type->data.unionation.layout != ContainerLayoutAuto) {
        const char *qual_str;
        switch (union_type->data.unionation.layout) {
            case ContainerLayoutAuto:
                zig_unreachable();
            case ContainerLayoutPacked:
                qual_str = "packed";
                break;
            case ContainerLayoutExtern:
                qual_str = "extern";
                break;
        }
        AstNode *source_node = (decl_node->data.container_decl.init_arg_expr != nullptr) ?
            decl_node->data.container_decl.init_arg_expr : decl_node;
        add_node_error(g, source_node,
            buf_sprintf("%s union does not support enum tag type", qual_str));
        union_type->data.unionation.resolve_status = ResolveStatusInvalid;
        return ErrorSemanticAnalyzeFail;
    }

    if (create_enum_type) {
        // Now iterate again and populate the unspecified tag values
        uint32_t next_maybe_unoccupied_index = 0;

        for (uint32_t field_i = 0; field_i < field_count; field_i += 1) {
            AstNode *field_node = decl_node->data.container_decl.fields.at(field_i);
            TypeUnionField *union_field = &union_type->data.unionation.fields[field_i];
            AstNode *tag_value = field_node->data.struct_field.value;

            if (tag_value == nullptr) {
                if (occupied_tag_values.size() == 0) {
                    bigint_init_unsigned(&union_field->enum_field->value, next_maybe_unoccupied_index);
                    next_maybe_unoccupied_index += 1;
                } else {
                    BigInt proposed_value;
                    for (;;) {
                        bigint_init_unsigned(&proposed_value, next_maybe_unoccupied_index);
                        next_maybe_unoccupied_index += 1;
                        auto entry = occupied_tag_values.put_unique(proposed_value, field_node);
                        if (entry != nullptr) {
                            continue;
                        }
                        break;
                    }
                    bigint_init_bigint(&union_field->enum_field->value, &proposed_value);
                }
            }
        }
    } else if (enum_type_node != nullptr) {
        for (uint32_t i = 0; i < tag_type->data.enumeration.src_field_count; i += 1) {
            TypeEnumField *enum_field = &tag_type->data.enumeration.fields[i];
            if (!covered_enum_fields[i]) {
                AstNode *enum_decl_node = tag_type->data.enumeration.decl_node;
                AstNode *field_node = enum_decl_node->data.container_decl.fields.at(i);
                ErrorMsg *msg = add_node_error(g, decl_node,
                    buf_sprintf("enum field missing: '%s'", buf_ptr(enum_field->name)));
                add_error_note(g, msg, field_node,
                        buf_sprintf("declared here"));
                union_type->data.unionation.resolve_status = ResolveStatusInvalid;
            }
        }
    }

    if (union_type->data.unionation.resolve_status == ResolveStatusInvalid) {
        return ErrorSemanticAnalyzeFail;
    }

    union_type->data.unionation.resolve_loop_flag_zero_bits = false;

    union_type->data.unionation.gen_field_count = gen_field_index;
    bool zero_bits = gen_field_index == 0 && (field_count < 2 || !src_have_tag);
    if (!zero_bits) {
        union_type->abi_size = SIZE_MAX;
        union_type->size_in_bits = SIZE_MAX;
    }
    union_type->data.unionation.resolve_status = zero_bits ? ResolveStatusSizeKnown : ResolveStatusZeroBitsKnown;

    return ErrorNone;
}

void append_namespace_qualification(CodeGen *g, Buf *buf, ZigType *container_type) {
    if (g->root_import == container_type || buf_len(&container_type->name) == 0) return;
    buf_append_buf(buf, &container_type->name);
    buf_append_char(buf, NAMESPACE_SEP_CHAR);
}

static void get_fully_qualified_decl_name(CodeGen *g, Buf *buf, Tld *tld, bool is_test) {
    buf_resize(buf, 0);

    Scope *scope = tld->parent_scope;
    while (scope->id != ScopeIdDecls) {
        scope = scope->parent;
    }
    ScopeDecls *decls_scope = reinterpret_cast<ScopeDecls *>(scope);
    append_namespace_qualification(g, buf, decls_scope->container_type);
    if (is_test) {
        buf_append_str(buf, "test \"");
        buf_append_buf(buf, tld->name);
        buf_append_char(buf, '"');
    } else {
        buf_append_buf(buf, tld->name);
    }
}

ZigFn *create_fn_raw(CodeGen *g, FnInline inline_value) {
    ZigFn *fn_entry = allocate<ZigFn>(1);

    fn_entry->prealloc_backward_branch_quota = default_backward_branch_quota;

    fn_entry->analyzed_executable.backward_branch_count = &fn_entry->prealloc_bbc;
    fn_entry->analyzed_executable.backward_branch_quota = &fn_entry->prealloc_backward_branch_quota;
    fn_entry->analyzed_executable.fn_entry = fn_entry;
    fn_entry->ir_executable.fn_entry = fn_entry;
    fn_entry->fn_inline = inline_value;

    return fn_entry;
}

ZigFn *create_fn(CodeGen *g, AstNode *proto_node) {
    assert(proto_node->type == NodeTypeFnProto);
    AstNodeFnProto *fn_proto = &proto_node->data.fn_proto;

    ZigFn *fn_entry = create_fn_raw(g, fn_proto->fn_inline);

    fn_entry->proto_node = proto_node;
    fn_entry->body_node = (proto_node->data.fn_proto.fn_def_node == nullptr) ? nullptr :
        proto_node->data.fn_proto.fn_def_node->data.fn_def.body;

    fn_entry->analyzed_executable.source_node = fn_entry->body_node;

    return fn_entry;
}

static bool scope_is_root_decls(Scope *scope) {
    while (scope) {
        if (scope->id == ScopeIdDecls) {
            ScopeDecls *scope_decls = (ScopeDecls *)scope;
            return is_top_level_struct(scope_decls->container_type);
        }
        scope = scope->parent;
    }
    zig_unreachable();
}

ZigType *get_test_fn_type(CodeGen *g) {
    if (g->test_fn_type)
        return g->test_fn_type;

    FnTypeId fn_type_id = {0};
    fn_type_id.return_type = get_error_union_type(g, g->builtin_types.entry_global_error_set,
            g->builtin_types.entry_void);
    g->test_fn_type = get_fn_type(g, &fn_type_id);
    return g->test_fn_type;
}

void add_var_export(CodeGen *g, ZigVar *var, const char *symbol_name, GlobalLinkageId linkage) {
    GlobalExport *global_export = var->export_list.add_one();
    memset(global_export, 0, sizeof(GlobalExport));
    buf_init_from_str(&global_export->name, symbol_name);
    global_export->linkage = linkage;
}

void add_fn_export(CodeGen *g, ZigFn *fn_table_entry, const char *symbol_name, GlobalLinkageId linkage, bool ccc) {
    if (ccc) {
        if (strcmp(symbol_name, "main") == 0 && g->libc_link_lib != nullptr) {
            g->have_c_main = true;
        } else if (strcmp(symbol_name, "WinMain") == 0 &&
            g->zig_target->os == OsWindows)
        {
            g->have_winmain = true;
        } else if (strcmp(symbol_name, "WinMainCRTStartup") == 0 &&
            g->zig_target->os == OsWindows)
        {
            g->have_winmain_crt_startup = true;
        } else if (strcmp(symbol_name, "DllMainCRTStartup") == 0 &&
            g->zig_target->os == OsWindows)
        {
            g->have_dllmain_crt_startup = true;
        }
    }

    GlobalExport *fn_export = fn_table_entry->export_list.add_one();
    memset(fn_export, 0, sizeof(GlobalExport));
    buf_init_from_str(&fn_export->name, symbol_name);
    fn_export->linkage = linkage;
}

static void resolve_decl_fn(CodeGen *g, TldFn *tld_fn) {
    ZigType *import = tld_fn->base.import;
    AstNode *source_node = tld_fn->base.source_node;
    if (source_node->type == NodeTypeFnProto) {
        AstNodeFnProto *fn_proto = &source_node->data.fn_proto;

        AstNode *fn_def_node = fn_proto->fn_def_node;

        ZigFn *fn_table_entry = create_fn(g, source_node);
        tld_fn->fn_entry = fn_table_entry;

        bool is_extern = (fn_table_entry->body_node == nullptr);
        if (fn_proto->is_export || is_extern) {
            buf_init_from_buf(&fn_table_entry->symbol_name, tld_fn->base.name);
        } else {
            get_fully_qualified_decl_name(g, &fn_table_entry->symbol_name, &tld_fn->base, false);
        }

        if (fn_proto->is_export) {
            bool ccc = (fn_proto->cc == CallingConventionUnspecified || fn_proto->cc == CallingConventionC);
            add_fn_export(g, fn_table_entry, buf_ptr(&fn_table_entry->symbol_name), GlobalLinkageIdStrong, ccc);
        }

        if (!is_extern) {
            fn_table_entry->fndef_scope = create_fndef_scope(g,
                fn_table_entry->body_node, tld_fn->base.parent_scope, fn_table_entry);

            for (size_t i = 0; i < fn_proto->params.length; i += 1) {
                AstNode *param_node = fn_proto->params.at(i);
                assert(param_node->type == NodeTypeParamDecl);
                if (param_node->data.param_decl.name == nullptr) {
                    add_node_error(g, param_node, buf_sprintf("missing parameter name"));
                }
            }
        } else {
            fn_table_entry->inferred_async_node = inferred_async_none;
            g->external_prototypes.put_unique(tld_fn->base.name, &tld_fn->base);
        }

        Scope *child_scope = fn_table_entry->fndef_scope ? &fn_table_entry->fndef_scope->base : tld_fn->base.parent_scope;

        fn_table_entry->type_entry = analyze_fn_type(g, source_node, child_scope, fn_table_entry);

        if (fn_proto->section_expr != nullptr) {
            if (!analyze_const_string(g, child_scope, fn_proto->section_expr, &fn_table_entry->section_name)) {
                fn_table_entry->type_entry = g->builtin_types.entry_invalid;
            }
        }

        if (fn_table_entry->type_entry->id == ZigTypeIdInvalid) {
            tld_fn->base.resolution = TldResolutionInvalid;
            return;
        }

        if (!fn_table_entry->type_entry->data.fn.is_generic) {
            if (fn_def_node)
                g->fn_defs.append(fn_table_entry);
        }

        // if the calling convention implies that it cannot be async, we save that for later
        // and leave the value to be nullptr to indicate that we have not emitted possible
        // compile errors for improperly calling async functions.
        if (fn_table_entry->type_entry->data.fn.fn_type_id.cc == CallingConventionAsync) {
            fn_table_entry->inferred_async_node = fn_table_entry->proto_node;
        }

        if (scope_is_root_decls(tld_fn->base.parent_scope) && import == g->root_import) {
            if (g->have_pub_main && buf_eql_str(tld_fn->base.name, "main")) {
                g->main_fn = fn_table_entry;
            }
        }
    } else if (source_node->type == NodeTypeTestDecl) {
        ZigFn *fn_table_entry = create_fn_raw(g, FnInlineAuto);

        get_fully_qualified_decl_name(g, &fn_table_entry->symbol_name, &tld_fn->base, true);

        tld_fn->fn_entry = fn_table_entry;

        fn_table_entry->proto_node = source_node;
        fn_table_entry->fndef_scope = create_fndef_scope(g, source_node, tld_fn->base.parent_scope, fn_table_entry);
        fn_table_entry->type_entry = get_test_fn_type(g);
        fn_table_entry->body_node = source_node->data.test_decl.body;
        fn_table_entry->is_test = true;

        g->fn_defs.append(fn_table_entry);
        g->test_fns.append(fn_table_entry);

    } else {
        zig_unreachable();
    }
}

static void resolve_decl_comptime(CodeGen *g, TldCompTime *tld_comptime) {
    assert(tld_comptime->base.source_node->type == NodeTypeCompTime);
    AstNode *expr_node = tld_comptime->base.source_node->data.comptime_expr.expr;
    analyze_const_value(g, tld_comptime->base.parent_scope, expr_node, g->builtin_types.entry_void,
            nullptr, UndefBad);
}

static void add_top_level_decl(CodeGen *g, ScopeDecls *decls_scope, Tld *tld) {
    bool is_export = false;
    if (tld->id == TldIdVar) {
        assert(tld->source_node->type == NodeTypeVariableDeclaration);
        is_export = tld->source_node->data.variable_declaration.is_export;
    } else if (tld->id == TldIdFn) {
        assert(tld->source_node->type == NodeTypeFnProto);
        is_export = tld->source_node->data.fn_proto.is_export;

        if (!is_export && !tld->source_node->data.fn_proto.is_extern &&
            tld->source_node->data.fn_proto.fn_def_node == nullptr)
        {
            add_node_error(g, tld->source_node, buf_sprintf("non-extern function has no body"));
            return;
        }
    } else if (tld->id == TldIdUsingNamespace) {
        g->resolve_queue.append(tld);
    }
    if (is_export) {
        g->resolve_queue.append(tld);

        auto entry = g->exported_symbol_names.put_unique(tld->name, tld);
        if (entry) {
            AstNode *other_source_node = entry->value->source_node;
            ErrorMsg *msg = add_node_error(g, tld->source_node,
                    buf_sprintf("exported symbol collision: '%s'", buf_ptr(tld->name)));
            add_error_note(g, msg, other_source_node, buf_sprintf("other symbol here"));
        }
    }

    if (tld->name != nullptr) {
        auto entry = decls_scope->decl_table.put_unique(tld->name, tld);
        if (entry) {
            Tld *other_tld = entry->value;
            ErrorMsg *msg = add_node_error(g, tld->source_node, buf_sprintf("redefinition of '%s'", buf_ptr(tld->name)));
            add_error_note(g, msg, other_tld->source_node, buf_sprintf("previous definition is here"));
            return;
        }

        ZigType *type;
        if (get_primitive_type(g, tld->name, &type) != ErrorPrimitiveTypeNotFound) {
            add_node_error(g, tld->source_node,
                buf_sprintf("declaration shadows primitive type '%s'", buf_ptr(tld->name)));
        }
    }
}

static void preview_test_decl(CodeGen *g, AstNode *node, ScopeDecls *decls_scope) {
    assert(node->type == NodeTypeTestDecl);

    if (!g->is_test_build)
        return;

    ZigType *import = get_scope_import(&decls_scope->base);
    if (import->data.structure.root_struct->package != g->root_package)
        return;

    Buf *decl_name_buf = node->data.test_decl.name;

    Buf *test_name = g->test_name_prefix ?
        buf_sprintf("%s%s", buf_ptr(g->test_name_prefix), buf_ptr(decl_name_buf)) : decl_name_buf;

    if (g->test_filter != nullptr && strstr(buf_ptr(test_name), buf_ptr(g->test_filter)) == nullptr) {
        return;
    }

    TldFn *tld_fn = allocate<TldFn>(1);
    init_tld(&tld_fn->base, TldIdFn, test_name, VisibModPrivate, node, &decls_scope->base);
    g->resolve_queue.append(&tld_fn->base);
}

static void preview_comptime_decl(CodeGen *g, AstNode *node, ScopeDecls *decls_scope) {
    assert(node->type == NodeTypeCompTime);

    TldCompTime *tld_comptime = allocate<TldCompTime>(1);
    init_tld(&tld_comptime->base, TldIdCompTime, nullptr, VisibModPrivate, node, &decls_scope->base);
    g->resolve_queue.append(&tld_comptime->base);
}

void init_tld(Tld *tld, TldId id, Buf *name, VisibMod visib_mod, AstNode *source_node,
    Scope *parent_scope)
{
    tld->id = id;
    tld->name = name;
    tld->visib_mod = visib_mod;
    tld->source_node = source_node;
    tld->import = source_node ? source_node->owner : nullptr;
    tld->parent_scope = parent_scope;
}

void update_compile_var(CodeGen *g, Buf *name, ConstExprValue *value) {
    Tld *tld = get_container_scope(g->compile_var_import)->decl_table.get(name);
    resolve_top_level_decl(g, tld, tld->source_node, false);
    assert(tld->id == TldIdVar);
    TldVar *tld_var = (TldVar *)tld;
    tld_var->var->const_value = value;
    tld_var->var->var_type = value->type;
    tld_var->var->align_bytes = get_abi_alignment(g, value->type);
}

void scan_decls(CodeGen *g, ScopeDecls *decls_scope, AstNode *node) {
    switch (node->type) {
        case NodeTypeContainerDecl:
            for (size_t i = 0; i < node->data.container_decl.decls.length; i += 1) {
                AstNode *child = node->data.container_decl.decls.at(i);
                scan_decls(g, decls_scope, child);
            }
            break;
        case NodeTypeFnDef:
            scan_decls(g, decls_scope, node->data.fn_def.fn_proto);
            break;
        case NodeTypeVariableDeclaration:
            {
                Buf *name = node->data.variable_declaration.symbol;
                VisibMod visib_mod = node->data.variable_declaration.visib_mod;
                TldVar *tld_var = allocate<TldVar>(1);
                init_tld(&tld_var->base, TldIdVar, name, visib_mod, node, &decls_scope->base);
                tld_var->extern_lib_name = node->data.variable_declaration.lib_name;
                add_top_level_decl(g, decls_scope, &tld_var->base);
                break;
            }
        case NodeTypeFnProto:
            {
                // if the name is missing, we immediately announce an error
                Buf *fn_name = node->data.fn_proto.name;
                if (fn_name == nullptr) {
                    add_node_error(g, node, buf_sprintf("missing function name"));
                    break;
                }

                VisibMod visib_mod = node->data.fn_proto.visib_mod;
                TldFn *tld_fn = allocate<TldFn>(1);
                init_tld(&tld_fn->base, TldIdFn, fn_name, visib_mod, node, &decls_scope->base);
                tld_fn->extern_lib_name = node->data.fn_proto.lib_name;
                add_top_level_decl(g, decls_scope, &tld_fn->base);

                break;
            }
        case NodeTypeUsingNamespace: {
            VisibMod visib_mod = node->data.using_namespace.visib_mod;
            TldUsingNamespace *tld_using_namespace = allocate<TldUsingNamespace>(1);
            init_tld(&tld_using_namespace->base, TldIdUsingNamespace, nullptr, visib_mod, node, &decls_scope->base);
            add_top_level_decl(g, decls_scope, &tld_using_namespace->base);
            decls_scope->use_decls.append(tld_using_namespace);
            break;
        }
        case NodeTypeTestDecl:
            preview_test_decl(g, node, decls_scope);
            break;
        case NodeTypeCompTime:
            preview_comptime_decl(g, node, decls_scope);
            break;
        case NodeTypeParamDecl:
        case NodeTypeReturnExpr:
        case NodeTypeDefer:
        case NodeTypeBlock:
        case NodeTypeGroupedExpr:
        case NodeTypeBinOpExpr:
        case NodeTypeCatchExpr:
        case NodeTypeFnCallExpr:
        case NodeTypeArrayAccessExpr:
        case NodeTypeSliceExpr:
        case NodeTypeFloatLiteral:
        case NodeTypeIntLiteral:
        case NodeTypeStringLiteral:
        case NodeTypeCharLiteral:
        case NodeTypeBoolLiteral:
        case NodeTypeNullLiteral:
        case NodeTypeUndefinedLiteral:
        case NodeTypeSymbol:
        case NodeTypePrefixOpExpr:
        case NodeTypePointerType:
        case NodeTypeIfBoolExpr:
        case NodeTypeWhileExpr:
        case NodeTypeForExpr:
        case NodeTypeSwitchExpr:
        case NodeTypeSwitchProng:
        case NodeTypeSwitchRange:
        case NodeTypeBreak:
        case NodeTypeContinue:
        case NodeTypeUnreachable:
        case NodeTypeAsmExpr:
        case NodeTypeFieldAccessExpr:
        case NodeTypePtrDeref:
        case NodeTypeUnwrapOptional:
        case NodeTypeStructField:
        case NodeTypeContainerInitExpr:
        case NodeTypeStructValueField:
        case NodeTypeArrayType:
        case NodeTypeInferredArrayType:
        case NodeTypeErrorType:
        case NodeTypeIfErrorExpr:
        case NodeTypeIfOptional:
        case NodeTypeErrorSetDecl:
        case NodeTypeResume:
        case NodeTypeAwaitExpr:
        case NodeTypeSuspend:
        case NodeTypeEnumLiteral:
        case NodeTypeAnyFrameType:
        case NodeTypeErrorSetField:
            zig_unreachable();
    }
}

static Error resolve_decl_container(CodeGen *g, TldContainer *tld_container) {
    ZigType *type_entry = tld_container->type_entry;
    assert(type_entry);

    switch (type_entry->id) {
        case ZigTypeIdStruct:
            return resolve_struct_type(g, tld_container->type_entry);
        case ZigTypeIdEnum:
            return resolve_enum_zero_bits(g, tld_container->type_entry);
        case ZigTypeIdUnion:
            return resolve_union_type(g, tld_container->type_entry);
        default:
            zig_unreachable();
    }
}

ZigType *validate_var_type(CodeGen *g, AstNode *source_node, ZigType *type_entry) {
    switch (type_entry->id) {
        case ZigTypeIdInvalid:
            return g->builtin_types.entry_invalid;
        case ZigTypeIdUnreachable:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdArgTuple:
        case ZigTypeIdOpaque:
            add_node_error(g, source_node, buf_sprintf("variable of type '%s' not allowed",
                buf_ptr(&type_entry->name)));
            return g->builtin_types.entry_invalid;
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdMetaType:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdInt:
        case ZigTypeIdFloat:
        case ZigTypeIdPointer:
        case ZigTypeIdArray:
        case ZigTypeIdStruct:
        case ZigTypeIdOptional:
        case ZigTypeIdErrorUnion:
        case ZigTypeIdErrorSet:
        case ZigTypeIdEnum:
        case ZigTypeIdUnion:
        case ZigTypeIdFn:
        case ZigTypeIdBoundFn:
        case ZigTypeIdVector:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            return type_entry;
    }
    zig_unreachable();
}

// Set name to nullptr to make the variable anonymous (not visible to programmer).
// TODO merge with definition of add_local_var in ir.cpp
ZigVar *add_variable(CodeGen *g, AstNode *source_node, Scope *parent_scope, Buf *name,
    bool is_const, ConstExprValue *const_value, Tld *src_tld, ZigType *var_type)
{
    Error err;
    assert(const_value != nullptr);
    assert(var_type != nullptr);

    ZigVar *variable_entry = allocate<ZigVar>(1);
    variable_entry->const_value = const_value;
    variable_entry->var_type = var_type;
    variable_entry->parent_scope = parent_scope;
    variable_entry->shadowable = false;
    variable_entry->mem_slot_index = SIZE_MAX;
    variable_entry->src_arg_index = SIZE_MAX;

    assert(name);
    variable_entry->name = strdup(buf_ptr(name));

    if ((err = type_resolve(g, var_type, ResolveStatusAlignmentKnown))) {
        variable_entry->var_type = g->builtin_types.entry_invalid;
    } else {
        variable_entry->align_bytes = get_abi_alignment(g, var_type);

        ZigVar *existing_var = find_variable(g, parent_scope, name, nullptr);
        if (existing_var && !existing_var->shadowable) {
            if (existing_var->var_type == nullptr || !type_is_invalid(existing_var->var_type)) {
                ErrorMsg *msg = add_node_error(g, source_node,
                        buf_sprintf("redeclaration of variable '%s'", buf_ptr(name)));
                add_error_note(g, msg, existing_var->decl_node, buf_sprintf("previous declaration is here"));
            }
            variable_entry->var_type = g->builtin_types.entry_invalid;
        } else {
            ZigType *type;
            if (get_primitive_type(g, name, &type) != ErrorPrimitiveTypeNotFound) {
                add_node_error(g, source_node,
                        buf_sprintf("variable shadows primitive type '%s'", buf_ptr(name)));
                variable_entry->var_type = g->builtin_types.entry_invalid;
            } else {
                Scope *search_scope = nullptr;
                if (src_tld == nullptr) {
                    search_scope = parent_scope;
                } else if (src_tld->parent_scope != nullptr && src_tld->parent_scope->parent != nullptr) {
                    search_scope = src_tld->parent_scope->parent;
                }
                if (search_scope != nullptr) {
                    Tld *tld = find_decl(g, search_scope, name);
                    if (tld != nullptr && tld != src_tld) {
                        ErrorMsg *msg = add_node_error(g, source_node,
                                buf_sprintf("redefinition of '%s'", buf_ptr(name)));
                        add_error_note(g, msg, tld->source_node, buf_sprintf("previous definition is here"));
                        variable_entry->var_type = g->builtin_types.entry_invalid;
                    }
                }
            }
        }
    }

    Scope *child_scope;
    if (source_node && source_node->type == NodeTypeParamDecl) {
        child_scope = create_var_scope(g, source_node, parent_scope, variable_entry);
    } else {
        // it's already in the decls table
        child_scope = parent_scope;
    }


    variable_entry->src_is_const = is_const;
    variable_entry->gen_is_const = is_const;
    variable_entry->decl_node = source_node;
    variable_entry->child_scope = child_scope;


    return variable_entry;
}

static void resolve_decl_var(CodeGen *g, TldVar *tld_var, bool allow_lazy) {
    AstNode *source_node = tld_var->base.source_node;
    AstNodeVariableDeclaration *var_decl = &source_node->data.variable_declaration;

    bool is_const = var_decl->is_const;
    bool is_extern = var_decl->is_extern;
    bool is_export = var_decl->is_export;
    bool is_thread_local = var_decl->threadlocal_tok != nullptr;

    ZigType *explicit_type = nullptr;
    if (var_decl->type) {
        if (tld_var->analyzing_type) {
            add_node_error(g, var_decl->type,
                buf_sprintf("type of '%s' depends on itself", buf_ptr(tld_var->base.name)));
            explicit_type = g->builtin_types.entry_invalid;
        } else {
            tld_var->analyzing_type = true;
            ZigType *proposed_type = analyze_type_expr(g, tld_var->base.parent_scope, var_decl->type);
            explicit_type = validate_var_type(g, var_decl->type, proposed_type);
        }
    }

    assert(!is_export || !is_extern);

    ConstExprValue *init_value = nullptr;

    // TODO more validation for types that can't be used for export/extern variables
    ZigType *implicit_type = nullptr;
    if (explicit_type && explicit_type->id == ZigTypeIdInvalid) {
        implicit_type = explicit_type;
    } else if (var_decl->expr) {
        init_value = analyze_const_value(g, tld_var->base.parent_scope, var_decl->expr, explicit_type,
                var_decl->symbol, allow_lazy ? LazyOk : UndefOk);
        assert(init_value);
        implicit_type = init_value->type;

        if (implicit_type->id == ZigTypeIdUnreachable) {
            add_node_error(g, source_node, buf_sprintf("variable initialization is unreachable"));
            implicit_type = g->builtin_types.entry_invalid;
        } else if ((!is_const || is_extern) &&
                (implicit_type->id == ZigTypeIdComptimeFloat ||
                implicit_type->id == ZigTypeIdComptimeInt ||
                implicit_type->id == ZigTypeIdEnumLiteral))
        {
            add_node_error(g, source_node, buf_sprintf("unable to infer variable type"));
            implicit_type = g->builtin_types.entry_invalid;
        } else if (implicit_type->id == ZigTypeIdNull) {
            add_node_error(g, source_node, buf_sprintf("unable to infer variable type"));
            implicit_type = g->builtin_types.entry_invalid;
        } else if (implicit_type->id == ZigTypeIdMetaType && !is_const) {
            add_node_error(g, source_node, buf_sprintf("variable of type 'type' must be constant"));
            implicit_type = g->builtin_types.entry_invalid;
        }
        assert(implicit_type->id == ZigTypeIdInvalid || init_value->special != ConstValSpecialRuntime);
    } else if (!is_extern) {
        add_node_error(g, source_node, buf_sprintf("variables must be initialized"));
        implicit_type = g->builtin_types.entry_invalid;
    }

    ZigType *type = explicit_type ? explicit_type : implicit_type;
    assert(type != nullptr); // should have been caught by the parser

    ConstExprValue *init_val = (init_value != nullptr) ? init_value : create_const_runtime(type);

    tld_var->var = add_variable(g, source_node, tld_var->base.parent_scope, var_decl->symbol,
            is_const, init_val, &tld_var->base, type);
    tld_var->var->is_thread_local = is_thread_local;

    if (implicit_type != nullptr && type_is_invalid(implicit_type)) {
        tld_var->var->var_type = g->builtin_types.entry_invalid;
    }

    if (var_decl->align_expr != nullptr) {
        if (!analyze_const_align(g, tld_var->base.parent_scope, var_decl->align_expr, &tld_var->var->align_bytes)) {
            tld_var->var->var_type = g->builtin_types.entry_invalid;
        }
    }

    if (var_decl->section_expr != nullptr) {
        if (!analyze_const_string(g, tld_var->base.parent_scope, var_decl->section_expr, &tld_var->section_name)) {
            tld_var->section_name = nullptr;
        }
    }

    if (is_thread_local && is_const) {
        add_node_error(g, source_node, buf_sprintf("threadlocal variable cannot be constant"));
    }

    if (is_export) {
        add_var_export(g, tld_var->var, tld_var->var->name, GlobalLinkageIdStrong);
    }

    g->global_vars.append(tld_var);
}

static void add_symbols_from_container(CodeGen *g, TldUsingNamespace *src_using_namespace,
        TldUsingNamespace *dst_using_namespace, ScopeDecls* dest_decls_scope)
{
    if (src_using_namespace->base.resolution == TldResolutionUnresolved ||
        src_using_namespace->base.resolution == TldResolutionResolving)
    {
        assert(src_using_namespace->base.parent_scope->id == ScopeIdDecls);
        ScopeDecls *src_decls_scope = (ScopeDecls *)src_using_namespace->base.parent_scope;
        preview_use_decl(g, src_using_namespace, src_decls_scope);
        if (src_using_namespace != dst_using_namespace) {
            resolve_use_decl(g, src_using_namespace, src_decls_scope);
        }
    }

    ConstExprValue *use_expr = src_using_namespace->using_namespace_value;
    if (type_is_invalid(use_expr->type)) {
        dest_decls_scope->any_imports_failed = true;
        return;
    }

    dst_using_namespace->base.resolution = TldResolutionOk;

    assert(use_expr->special != ConstValSpecialRuntime);

    // The source scope for the imported symbols
    ScopeDecls *src_scope = get_container_scope(use_expr->data.x_type);
    // The top-level container where the symbols are defined, it's used in the
    // loop below in order to exclude the ones coming from an import statement
    ZigType *src_import = get_scope_import(&src_scope->base);
    assert(src_import != nullptr);

    if (src_scope->any_imports_failed) {
        dest_decls_scope->any_imports_failed = true;
    }

    auto it = src_scope->decl_table.entry_iterator();
    for (;;) {
        auto *entry = it.next();
        if (!entry)
            break;

        Buf *target_tld_name = entry->key;
        Tld *target_tld = entry->value;

        if (target_tld->visib_mod == VisibModPrivate) {
            continue;
        }

        if (target_tld->import != src_import) {
            continue;
        }

        auto existing_entry = dest_decls_scope->decl_table.put_unique(target_tld_name, target_tld);
        if (existing_entry) {
            Tld *existing_decl = existing_entry->value;
            if (existing_decl != target_tld) {
                ErrorMsg *msg = add_node_error(g, dst_using_namespace->base.source_node,
                        buf_sprintf("import of '%s' overrides existing definition",
                            buf_ptr(target_tld_name)));
                add_error_note(g, msg, existing_decl->source_node, buf_sprintf("previous definition here"));
                add_error_note(g, msg, target_tld->source_node, buf_sprintf("imported definition here"));
            }
        }
    }

    for (size_t i = 0; i < src_scope->use_decls.length; i += 1) {
        TldUsingNamespace *tld_using_namespace = src_scope->use_decls.at(i);
        if (tld_using_namespace->base.visib_mod != VisibModPrivate)
            add_symbols_from_container(g, tld_using_namespace, dst_using_namespace, dest_decls_scope);
    }
}

static void resolve_use_decl(CodeGen *g, TldUsingNamespace *tld_using_namespace, ScopeDecls *dest_decls_scope) {
    if (tld_using_namespace->base.resolution == TldResolutionOk ||
        tld_using_namespace->base.resolution == TldResolutionInvalid)
    {
        return;
    }
    add_symbols_from_container(g, tld_using_namespace, tld_using_namespace, dest_decls_scope);
}

static void preview_use_decl(CodeGen *g, TldUsingNamespace *using_namespace, ScopeDecls *dest_decls_scope) {
    if (using_namespace->base.resolution == TldResolutionOk ||
        using_namespace->base.resolution == TldResolutionInvalid ||
        using_namespace->using_namespace_value != nullptr)
    {
        return;
    }

    using_namespace->base.resolution = TldResolutionResolving;
    assert(using_namespace->base.source_node->type == NodeTypeUsingNamespace);
    ConstExprValue *result = analyze_const_value(g, &dest_decls_scope->base,
        using_namespace->base.source_node->data.using_namespace.expr, g->builtin_types.entry_type,
        nullptr, UndefBad);
    using_namespace->using_namespace_value = result;

    if (type_is_invalid(result->type)) {
        dest_decls_scope->any_imports_failed = true;
        using_namespace->base.resolution = TldResolutionInvalid;
        using_namespace->using_namespace_value = &g->invalid_instruction->value;
        return;
    }

    if (!is_container(result->data.x_type)) {
        add_node_error(g, using_namespace->base.source_node,
            buf_sprintf("expected struct, enum, or union; found '%s'", buf_ptr(&result->data.x_type->name)));
        dest_decls_scope->any_imports_failed = true;
        using_namespace->base.resolution = TldResolutionInvalid;
        using_namespace->using_namespace_value = &g->invalid_instruction->value;
        return;
    }
}

void resolve_top_level_decl(CodeGen *g, Tld *tld, AstNode *source_node, bool allow_lazy) {
    bool want_resolve_lazy = tld->resolution == TldResolutionOkLazy && !allow_lazy;
    if (tld->resolution != TldResolutionUnresolved && !want_resolve_lazy)
        return;

    tld->resolution = TldResolutionResolving;
    update_progress_display(g);

    switch (tld->id) {
        case TldIdVar: {
            TldVar *tld_var = (TldVar *)tld;
            if (want_resolve_lazy) {
                ir_resolve_lazy(g, source_node, tld_var->var->const_value);
            } else {
                resolve_decl_var(g, tld_var, allow_lazy);
            }
            tld->resolution = allow_lazy ? TldResolutionOkLazy : TldResolutionOk;
            break;
        }
        case TldIdFn: {
            TldFn *tld_fn = (TldFn *)tld;
            resolve_decl_fn(g, tld_fn);

            tld->resolution = TldResolutionOk;
            break;
        }
        case TldIdContainer: {
            TldContainer *tld_container = (TldContainer *)tld;
            resolve_decl_container(g, tld_container);

            tld->resolution = TldResolutionOk;
            break;
        }
        case TldIdCompTime: {
            TldCompTime *tld_comptime = (TldCompTime *)tld;
            resolve_decl_comptime(g, tld_comptime);

            tld->resolution = TldResolutionOk;
            break;
        }
        case TldIdUsingNamespace: {
            TldUsingNamespace *tld_using_namespace = (TldUsingNamespace *)tld;
            assert(tld_using_namespace->base.parent_scope->id == ScopeIdDecls);
            ScopeDecls *dest_decls_scope = (ScopeDecls *)tld_using_namespace->base.parent_scope;
            preview_use_decl(g, tld_using_namespace, dest_decls_scope);
            resolve_use_decl(g, tld_using_namespace, dest_decls_scope);

            tld->resolution = TldResolutionOk;
            break;
        }
    }

    if (g->trace_err != nullptr && source_node != nullptr && !source_node->already_traced_this_node) {
        g->trace_err = add_error_note(g, g->trace_err, source_node, buf_create_from_str("referenced here"));
        source_node->already_traced_this_node = true;
    }
}

Tld *find_container_decl(CodeGen *g, ScopeDecls *decls_scope, Buf *name) {
    // resolve all the using_namespace decls
    for (size_t i = 0; i < decls_scope->use_decls.length; i += 1) {
        TldUsingNamespace *tld_using_namespace = decls_scope->use_decls.at(i);
        if (tld_using_namespace->base.resolution == TldResolutionUnresolved) {
            preview_use_decl(g, tld_using_namespace, decls_scope);
            resolve_use_decl(g, tld_using_namespace, decls_scope);
        }
    }

    auto entry = decls_scope->decl_table.maybe_get(name);
    return (entry == nullptr) ? nullptr : entry->value;
}

Tld *find_decl(CodeGen *g, Scope *scope, Buf *name) {
    while (scope) {
        if (scope->id == ScopeIdDecls) {
            ScopeDecls *decls_scope = (ScopeDecls *)scope;

            Tld *result = find_container_decl(g, decls_scope, name);
            if (result != nullptr)
                return result;
        }
        scope = scope->parent;
    }
    return nullptr;
}

ZigVar *find_variable(CodeGen *g, Scope *scope, Buf *name, ScopeFnDef **crossed_fndef_scope) {
    ScopeFnDef *my_crossed_fndef_scope = nullptr;
    while (scope) {
        if (scope->id == ScopeIdVarDecl) {
            ScopeVarDecl *var_scope = (ScopeVarDecl *)scope;
            if (buf_eql_str(name, var_scope->var->name)) {
                if (crossed_fndef_scope != nullptr)
                    *crossed_fndef_scope = my_crossed_fndef_scope;
                return var_scope->var;
            }
        } else if (scope->id == ScopeIdDecls) {
            ScopeDecls *decls_scope = (ScopeDecls *)scope;
            auto entry = decls_scope->decl_table.maybe_get(name);
            if (entry) {
                Tld *tld = entry->value;
                if (tld->id == TldIdVar) {
                    TldVar *tld_var = (TldVar *)tld;
                    if (tld_var->var) {
                        if (crossed_fndef_scope != nullptr)
                            *crossed_fndef_scope = nullptr;
                        return tld_var->var;
                    }
                }
            }
        } else if (scope->id == ScopeIdFnDef) {
            my_crossed_fndef_scope = (ScopeFnDef *)scope;
        }
        scope = scope->parent;
    }

    return nullptr;
}

ZigFn *scope_fn_entry(Scope *scope) {
    while (scope) {
        if (scope->id == ScopeIdFnDef) {
            ScopeFnDef *fn_scope = (ScopeFnDef *)scope;
            return fn_scope->fn_entry;
        }
        scope = scope->parent;
    }
    return nullptr;
}

ZigPackage *scope_package(Scope *scope) {
    ZigType *import = get_scope_import(scope);
    assert(is_top_level_struct(import));
    return import->data.structure.root_struct->package;
}

TypeEnumField *find_enum_type_field(ZigType *enum_type, Buf *name) {
    assert(enum_type->id == ZigTypeIdEnum);
    if (enum_type->data.enumeration.src_field_count == 0)
        return nullptr;
    auto entry = enum_type->data.enumeration.fields_by_name.maybe_get(name);
    if (entry == nullptr)
        return nullptr;
    return entry->value;
}

TypeStructField *find_struct_type_field(ZigType *type_entry, Buf *name) {
    assert(type_entry->id == ZigTypeIdStruct);
    assert(type_is_resolved(type_entry, ResolveStatusZeroBitsKnown));
    if (type_entry->data.structure.src_field_count == 0)
        return nullptr;
    auto entry = type_entry->data.structure.fields_by_name.maybe_get(name);
    if (entry == nullptr)
        return nullptr;
    return entry->value;
}

TypeUnionField *find_union_type_field(ZigType *type_entry, Buf *name) {
    assert(type_entry->id == ZigTypeIdUnion);
    assert(type_is_resolved(type_entry, ResolveStatusZeroBitsKnown));
    if (type_entry->data.unionation.src_field_count == 0)
        return nullptr;
    auto entry = type_entry->data.unionation.fields_by_name.maybe_get(name);
    if (entry == nullptr)
        return nullptr;
    return entry->value;
}

TypeUnionField *find_union_field_by_tag(ZigType *type_entry, const BigInt *tag) {
    assert(type_entry->id == ZigTypeIdUnion);
    assert(type_is_resolved(type_entry, ResolveStatusZeroBitsKnown));
    for (uint32_t i = 0; i < type_entry->data.unionation.src_field_count; i += 1) {
        TypeUnionField *field = &type_entry->data.unionation.fields[i];
        if (bigint_cmp(&field->enum_field->value, tag) == CmpEQ) {
            return field;
        }
    }
    return nullptr;
}

TypeEnumField *find_enum_field_by_tag(ZigType *enum_type, const BigInt *tag) {
    assert(type_is_resolved(enum_type, ResolveStatusZeroBitsKnown));
    for (uint32_t i = 0; i < enum_type->data.enumeration.src_field_count; i += 1) {
        TypeEnumField *field = &enum_type->data.enumeration.fields[i];
        if (bigint_cmp(&field->value, tag) == CmpEQ) {
            return field;
        }
    }
    return nullptr;
}


bool is_container(ZigType *type_entry) {
    switch (type_entry->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdStruct:
            return !type_entry->data.structure.is_slice;
        case ZigTypeIdEnum:
        case ZigTypeIdUnion:
            return true;
        case ZigTypeIdPointer:
        case ZigTypeIdMetaType:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdUnreachable:
        case ZigTypeIdInt:
        case ZigTypeIdFloat:
        case ZigTypeIdArray:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdOptional:
        case ZigTypeIdErrorUnion:
        case ZigTypeIdErrorSet:
        case ZigTypeIdFn:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
        case ZigTypeIdOpaque:
        case ZigTypeIdVector:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            return false;
    }
    zig_unreachable();
}

bool is_ref(ZigType *type_entry) {
    return type_entry->id == ZigTypeIdPointer && type_entry->data.pointer.ptr_len == PtrLenSingle;
}

bool is_array_ref(ZigType *type_entry) {
    ZigType *array = is_ref(type_entry) ?
        type_entry->data.pointer.child_type : type_entry;
    return array->id == ZigTypeIdArray;
}

bool is_container_ref(ZigType *parent_ty) {
    ZigType *ty = is_ref(parent_ty) ? parent_ty->data.pointer.child_type : parent_ty;
    return is_slice(ty) || is_container(ty);
}

ZigType *container_ref_type(ZigType *type_entry) {
    assert(is_container_ref(type_entry));
    return is_ref(type_entry) ?
        type_entry->data.pointer.child_type : type_entry;
}

ZigType *get_src_ptr_type(ZigType *type) {
    if (type->id == ZigTypeIdPointer) return type;
    if (type->id == ZigTypeIdFn) return type;
    if (type->id == ZigTypeIdAnyFrame) return type;
    if (type->id == ZigTypeIdOptional) {
        if (type->data.maybe.child_type->id == ZigTypeIdPointer) {
            return type->data.maybe.child_type->data.pointer.allow_zero ? nullptr : type->data.maybe.child_type;
        }
        if (type->data.maybe.child_type->id == ZigTypeIdFn) return type->data.maybe.child_type;
        if (type->data.maybe.child_type->id == ZigTypeIdAnyFrame) return type->data.maybe.child_type;
    }
    return nullptr;
}

ZigType *get_codegen_ptr_type(ZigType *type) {
    ZigType *ty = get_src_ptr_type(type);
    if (ty == nullptr || !type_has_bits(ty))
        return nullptr;
    return ty;
}

bool type_is_nonnull_ptr(ZigType *type) {
    return get_codegen_ptr_type(type) == type && !ptr_allows_addr_zero(type);
}

static uint32_t get_async_frame_align_bytes(CodeGen *g) {
    uint32_t a = g->pointer_size_bytes * 2;
    // promises have at least alignment 8 so that we can have 3 extra bits when doing atomicrmw
    if (a < 8) a = 8;
    return a;
}

uint32_t get_ptr_align(CodeGen *g, ZigType *type) {
    ZigType *ptr_type = get_src_ptr_type(type);
    if (ptr_type->id == ZigTypeIdPointer) {
        return (ptr_type->data.pointer.explicit_alignment == 0) ?
            get_abi_alignment(g, ptr_type->data.pointer.child_type) : ptr_type->data.pointer.explicit_alignment;
    } else if (ptr_type->id == ZigTypeIdFn) {
        // I tried making this use LLVMABIAlignmentOfType but it trips this assertion in LLVM:
        // "Cannot getTypeInfo() on a type that is unsized!"
        // when getting the alignment of `?extern fn() void`.
        // See http://lists.llvm.org/pipermail/llvm-dev/2018-September/126142.html
        return (ptr_type->data.fn.fn_type_id.alignment == 0) ? 1 : ptr_type->data.fn.fn_type_id.alignment;
    } else if (ptr_type->id == ZigTypeIdAnyFrame) {
        return get_async_frame_align_bytes(g);
    } else {
        zig_unreachable();
    }
}

bool get_ptr_const(ZigType *type) {
    ZigType *ptr_type = get_src_ptr_type(type);
    if (ptr_type->id == ZigTypeIdPointer) {
        return ptr_type->data.pointer.is_const;
    } else if (ptr_type->id == ZigTypeIdFn) {
        return true;
    } else if (ptr_type->id == ZigTypeIdAnyFrame) {
        return true;
    } else {
        zig_unreachable();
    }
}

AstNode *get_param_decl_node(ZigFn *fn_entry, size_t index) {
    if (fn_entry->param_source_nodes)
        return fn_entry->param_source_nodes[index];
    else if (fn_entry->proto_node)
        return fn_entry->proto_node->data.fn_proto.params.at(index);
    else
        return nullptr;
}

static void define_local_param_variables(CodeGen *g, ZigFn *fn_table_entry) {
    ZigType *fn_type = fn_table_entry->type_entry;
    assert(!fn_type->data.fn.is_generic);
    FnTypeId *fn_type_id = &fn_type->data.fn.fn_type_id;
    for (size_t i = 0; i < fn_type_id->param_count; i += 1) {
        FnTypeParamInfo *param_info = &fn_type_id->param_info[i];
        AstNode *param_decl_node = get_param_decl_node(fn_table_entry, i);
        Buf *param_name;
        bool is_var_args = param_decl_node && param_decl_node->data.param_decl.is_var_args;
        if (param_decl_node && !is_var_args) {
            param_name = param_decl_node->data.param_decl.name;
        } else {
            param_name = buf_sprintf("arg%" ZIG_PRI_usize "", i);
        }
        if (param_name == nullptr) {
            continue;
        }

        ZigType *param_type = param_info->type;
        bool is_noalias = param_info->is_noalias;

        if (is_noalias && get_codegen_ptr_type(param_type) == nullptr) {
            add_node_error(g, param_decl_node, buf_sprintf("noalias on non-pointer parameter"));
        }

        ZigVar *var = add_variable(g, param_decl_node, fn_table_entry->child_scope,
                param_name, true, create_const_runtime(param_type), nullptr, param_type);
        var->src_arg_index = i;
        fn_table_entry->child_scope = var->child_scope;
        var->shadowable = var->shadowable || is_var_args;

        if (type_has_bits(param_type)) {
            fn_table_entry->variable_list.append(var);
        }
    }
}

bool resolve_inferred_error_set(CodeGen *g, ZigType *err_set_type, AstNode *source_node) {
    assert(err_set_type->id == ZigTypeIdErrorSet);
    ZigFn *infer_fn = err_set_type->data.error_set.infer_fn;
    if (infer_fn != nullptr && err_set_type->data.error_set.incomplete) {
        if (infer_fn->anal_state == FnAnalStateInvalid) {
            return false;
        } else if (infer_fn->anal_state == FnAnalStateReady) {
            analyze_fn_body(g, infer_fn);
            if (err_set_type->data.error_set.incomplete) {
                assert(g->errors.length != 0);
                return false;
            }
        } else {
            add_node_error(g, source_node,
                buf_sprintf("cannot resolve inferred error set '%s': function '%s' not fully analyzed yet",
                    buf_ptr(&err_set_type->name), buf_ptr(&err_set_type->data.error_set.infer_fn->symbol_name)));
            return false;
        }
    }
    return true;
}

static void resolve_async_fn_frame(CodeGen *g, ZigFn *fn) {
    ZigType *frame_type = get_fn_frame_type(g, fn);
    Error err;
    if ((err = type_resolve(g, frame_type, ResolveStatusSizeKnown))) {
        if (g->trace_err != nullptr && frame_type->data.frame.resolve_loop_src_node != nullptr &&
            !frame_type->data.frame.reported_loop_err)
        {
            frame_type->data.frame.reported_loop_err = true;
            g->trace_err = add_error_note(g, g->trace_err, frame_type->data.frame.resolve_loop_src_node,
                buf_sprintf("when analyzing type '%s' here", buf_ptr(&frame_type->name)));
        }
        fn->anal_state = FnAnalStateInvalid;
        return;
    }
}

bool fn_is_async(ZigFn *fn) {
    assert(fn->inferred_async_node != nullptr);
    assert(fn->inferred_async_node != inferred_async_checking);
    return fn->inferred_async_node != inferred_async_none;
}

void add_async_error_notes(CodeGen *g, ErrorMsg *msg, ZigFn *fn) {
    assert(fn->inferred_async_node != nullptr);
    assert(fn->inferred_async_node != inferred_async_checking);
    assert(fn->inferred_async_node != inferred_async_none);
    if (fn->inferred_async_fn != nullptr) {
        ErrorMsg *new_msg;
        if (fn->inferred_async_node->type == NodeTypeAwaitExpr) {
            new_msg = add_error_note(g, msg, fn->inferred_async_node,
                    buf_create_from_str("await here is a suspend point"));
        } else {
            new_msg = add_error_note(g, msg, fn->inferred_async_node,
                buf_sprintf("async function call here"));
        }
        return add_async_error_notes(g, new_msg, fn->inferred_async_fn);
    } else if (fn->inferred_async_node->type == NodeTypeFnProto) {
        add_error_note(g, msg, fn->inferred_async_node,
            buf_sprintf("async calling convention here"));
    } else if (fn->inferred_async_node->type == NodeTypeSuspend) {
        add_error_note(g, msg, fn->inferred_async_node,
            buf_sprintf("suspends here"));
    } else if (fn->inferred_async_node->type == NodeTypeAwaitExpr) {
        add_error_note(g, msg, fn->inferred_async_node,
            buf_sprintf("await here is a suspend point"));
    } else if (fn->inferred_async_node->type == NodeTypeFnCallExpr &&
        fn->inferred_async_node->data.fn_call_expr.modifier == CallModifierBuiltin)
    {
        add_error_note(g, msg, fn->inferred_async_node,
            buf_sprintf("@frame() causes function to be async"));
    } else {
        add_error_note(g, msg, fn->inferred_async_node,
            buf_sprintf("suspends here"));
    }
}

// ErrorNone - not async
// ErrorIsAsync - yes async
// ErrorSemanticAnalyzeFail - compile error emitted result is invalid
static Error analyze_callee_async(CodeGen *g, ZigFn *fn, ZigFn *callee, AstNode *call_node,
        bool must_not_be_async, CallModifier modifier)
{
    if (modifier == CallModifierNoAsync)
        return ErrorNone;
    bool callee_is_async = false;
    switch (callee->type_entry->data.fn.fn_type_id.cc) {
        case CallingConventionUnspecified:
            break;
        case CallingConventionAsync:
            callee_is_async = true;
            break;
        default:
            return ErrorNone;
    }
    if (!callee_is_async) {
        if (callee->anal_state == FnAnalStateReady) {
            analyze_fn_body(g, callee);
            if (callee->anal_state == FnAnalStateInvalid) {
                return ErrorSemanticAnalyzeFail;
            }
        }
        if (callee->anal_state == FnAnalStateComplete) {
            analyze_fn_async(g, callee, true);
            if (callee->anal_state == FnAnalStateInvalid) {
                if (g->trace_err != nullptr) {
                    g->trace_err = add_error_note(g, g->trace_err, call_node,
                        buf_sprintf("while checking if '%s' is async", buf_ptr(&fn->symbol_name)));
                }
                return ErrorSemanticAnalyzeFail;
            }
            callee_is_async = fn_is_async(callee);
        } else {
            // If it's already been determined, use that value. Otherwise
            // assume non-async, emit an error later if it turned out to be async.
            if (callee->inferred_async_node == nullptr ||
                callee->inferred_async_node == inferred_async_checking)
            {
                callee->assumed_non_async = call_node;
                callee_is_async = false;
            } else {
                callee_is_async = callee->inferred_async_node != inferred_async_none;
            }
        }
    }
    if (callee_is_async) {
        bool bad_recursion = (fn->inferred_async_node == inferred_async_none);
        fn->inferred_async_node = call_node;
        fn->inferred_async_fn = callee;
        if (must_not_be_async) {
            ErrorMsg *msg = add_node_error(g, fn->proto_node,
                buf_sprintf("function with calling convention '%s' cannot be async",
                    calling_convention_name(fn->type_entry->data.fn.fn_type_id.cc)));
            add_async_error_notes(g, msg, fn);
            return ErrorSemanticAnalyzeFail;
        }
        if (bad_recursion) {
            ErrorMsg *msg = add_node_error(g, fn->proto_node,
                buf_sprintf("recursive function cannot be async"));
            add_async_error_notes(g, msg, fn);
            return ErrorSemanticAnalyzeFail;
        }
        if (fn->assumed_non_async != nullptr) {
            ErrorMsg *msg = add_node_error(g, fn->proto_node,
                buf_sprintf("unable to infer whether '%s' should be async",
                    buf_ptr(&fn->symbol_name)));
            add_error_note(g, msg, fn->assumed_non_async,
                buf_sprintf("assumed to be non-async here"));
            add_async_error_notes(g, msg, fn);
            fn->anal_state = FnAnalStateInvalid;
            return ErrorSemanticAnalyzeFail;
        }
        return ErrorIsAsync;
    }
    return ErrorNone;
}

// This function resolves functions being inferred async.
static void analyze_fn_async(CodeGen *g, ZigFn *fn, bool resolve_frame) {
    if (fn->inferred_async_node == inferred_async_checking) {
        // TODO call graph cycle detected, disallow the recursion
        fn->inferred_async_node = inferred_async_none;
        return;
    }
    if (fn->inferred_async_node == inferred_async_none) {
        return;
    }
    if (fn->inferred_async_node != nullptr) {
        if (resolve_frame) {
            resolve_async_fn_frame(g, fn);
        }
        return;
    }
    fn->inferred_async_node = inferred_async_checking;

    bool must_not_be_async = false;
    if (fn->type_entry->data.fn.fn_type_id.cc != CallingConventionUnspecified) {
        must_not_be_async = true;
        fn->inferred_async_node = inferred_async_none;
    }

    for (size_t i = 0; i < fn->call_list.length; i += 1) {
        IrInstructionCallGen *call = fn->call_list.at(i);
        if (call->fn_entry == nullptr) {
            // TODO function pointer call here, could be anything
            continue;
        }
        switch (analyze_callee_async(g, fn, call->fn_entry, call->base.source_node, must_not_be_async,
                    call->modifier))
        {
            case ErrorSemanticAnalyzeFail:
                fn->anal_state = FnAnalStateInvalid;
                return;
            case ErrorNone:
                continue;
            case ErrorIsAsync:
                if (resolve_frame) {
                    resolve_async_fn_frame(g, fn);
                }
                return;
            default:
                zig_unreachable();
        }
    }
    for (size_t i = 0; i < fn->await_list.length; i += 1) {
        IrInstructionAwaitGen *await = fn->await_list.at(i);
        // TODO If this is a noasync await, it doesn't count
        // https://github.com/ziglang/zig/issues/3157
        switch (analyze_callee_async(g, fn, await->target_fn, await->base.source_node, must_not_be_async,
                    CallModifierNone))
        {
            case ErrorSemanticAnalyzeFail:
                fn->anal_state = FnAnalStateInvalid;
                return;
            case ErrorNone:
                continue;
            case ErrorIsAsync:
                if (resolve_frame) {
                    resolve_async_fn_frame(g, fn);
                }
                return;
            default:
                zig_unreachable();
        }
    }
    fn->inferred_async_node = inferred_async_none;
}

static void analyze_fn_ir(CodeGen *g, ZigFn *fn, AstNode *return_type_node) {
    ZigType *fn_type = fn->type_entry;
    assert(!fn_type->data.fn.is_generic);
    FnTypeId *fn_type_id = &fn_type->data.fn.fn_type_id;

    ZigType *block_return_type = ir_analyze(g, &fn->ir_executable,
            &fn->analyzed_executable, fn_type_id->return_type, return_type_node);
    fn->src_implicit_return_type = block_return_type;

    if (type_is_invalid(block_return_type) || fn->analyzed_executable.first_err_trace_msg != nullptr) {
        assert(g->errors.length > 0);
        fn->anal_state = FnAnalStateInvalid;
        return;
    }

    if (fn_type_id->return_type->id == ZigTypeIdErrorUnion) {
        ZigType *return_err_set_type = fn_type_id->return_type->data.error_union.err_set_type;
        if (return_err_set_type->data.error_set.infer_fn != nullptr &&
            return_err_set_type->data.error_set.incomplete)
        {
            ZigType *inferred_err_set_type;
            if (fn->src_implicit_return_type->id == ZigTypeIdErrorSet) {
                inferred_err_set_type = fn->src_implicit_return_type;
            } else if (fn->src_implicit_return_type->id == ZigTypeIdErrorUnion) {
                inferred_err_set_type = fn->src_implicit_return_type->data.error_union.err_set_type;
            } else {
                add_node_error(g, return_type_node,
                        buf_sprintf("function with inferred error set must return at least one possible error"));
                fn->anal_state = FnAnalStateInvalid;
                return;
            }

            if (inferred_err_set_type->data.error_set.infer_fn != nullptr &&
                inferred_err_set_type->data.error_set.incomplete)
            {
                if (!resolve_inferred_error_set(g, inferred_err_set_type, return_type_node)) {
                    fn->anal_state = FnAnalStateInvalid;
                    return;
                }
            }

            return_err_set_type->data.error_set.incomplete = false;
            if (type_is_global_error_set(inferred_err_set_type)) {
                return_err_set_type->data.error_set.err_count = UINT32_MAX;
            } else {
                return_err_set_type->data.error_set.err_count = inferred_err_set_type->data.error_set.err_count;
                if (inferred_err_set_type->data.error_set.err_count > 0) {
                    return_err_set_type->data.error_set.errors = allocate<ErrorTableEntry *>(inferred_err_set_type->data.error_set.err_count);
                    for (uint32_t i = 0; i < inferred_err_set_type->data.error_set.err_count; i += 1) {
                        return_err_set_type->data.error_set.errors[i] = inferred_err_set_type->data.error_set.errors[i];
                    }
                }
            }
        }
    }

    CallingConvention cc = fn->type_entry->data.fn.fn_type_id.cc;
    if (cc != CallingConventionUnspecified && cc != CallingConventionAsync &&
        fn->inferred_async_node != nullptr &&
        fn->inferred_async_node != inferred_async_checking &&
        fn->inferred_async_node != inferred_async_none)
    {
        ErrorMsg *msg = add_node_error(g, fn->proto_node,
            buf_sprintf("function with calling convention '%s' cannot be async",
                calling_convention_name(cc)));
        add_async_error_notes(g, msg, fn);
        fn->anal_state = FnAnalStateInvalid;
    }

    if (g->verbose_ir) {
        fprintf(stderr, "fn %s() { // (analyzed)\n", buf_ptr(&fn->symbol_name));
        ir_print(g, stderr, &fn->analyzed_executable, 4, IrPassGen);
        fprintf(stderr, "}\n");
    }
    fn->anal_state = FnAnalStateComplete;
}

static void analyze_fn_body(CodeGen *g, ZigFn *fn_table_entry) {
    assert(fn_table_entry->anal_state != FnAnalStateProbing);
    if (fn_table_entry->anal_state != FnAnalStateReady)
        return;

    fn_table_entry->anal_state = FnAnalStateProbing;
    update_progress_display(g);

    AstNode *return_type_node = (fn_table_entry->proto_node != nullptr) ?
        fn_table_entry->proto_node->data.fn_proto.return_type : fn_table_entry->fndef_scope->base.source_node;

    assert(fn_table_entry->fndef_scope);
    if (!fn_table_entry->child_scope)
        fn_table_entry->child_scope = &fn_table_entry->fndef_scope->base;

    define_local_param_variables(g, fn_table_entry);

    ZigType *fn_type = fn_table_entry->type_entry;
    assert(!fn_type->data.fn.is_generic);

    ir_gen_fn(g, fn_table_entry);
    if (fn_table_entry->ir_executable.first_err_trace_msg != nullptr) {
        fn_table_entry->anal_state = FnAnalStateInvalid;
        return;
    }
    if (g->verbose_ir) {
        fprintf(stderr, "\n");
        ast_render(stderr, fn_table_entry->body_node, 4);
        fprintf(stderr, "\nfn %s() { // (IR)\n", buf_ptr(&fn_table_entry->symbol_name));
        ir_print(g, stderr, &fn_table_entry->ir_executable, 4, IrPassSrc);
        fprintf(stderr, "}\n");
    }

    analyze_fn_ir(g, fn_table_entry, return_type_node);
}

ZigType *add_source_file(CodeGen *g, ZigPackage *package, Buf *resolved_path, Buf *source_code,
        SourceKind source_kind)
{
    if (g->verbose_tokenize) {
        fprintf(stderr, "\nOriginal Source (%s):\n", buf_ptr(resolved_path));
        fprintf(stderr, "----------------\n");
        fprintf(stderr, "%s\n", buf_ptr(source_code));

        fprintf(stderr, "\nTokens:\n");
        fprintf(stderr, "---------\n");
    }

    Tokenization tokenization = {0};
    tokenize(source_code, &tokenization);

    if (tokenization.err) {
        ErrorMsg *err = err_msg_create_with_line(resolved_path, tokenization.err_line, tokenization.err_column,
                source_code, tokenization.line_offsets, tokenization.err);

        print_err_msg(err, g->err_color);
        exit(1);
    }

    if (g->verbose_tokenize) {
        print_tokens(source_code, tokenization.tokens);

        fprintf(stderr, "\nAST:\n");
        fprintf(stderr, "------\n");
    }

    Buf *src_dirname = buf_alloc();
    Buf *src_basename = buf_alloc();
    os_path_split(resolved_path, src_dirname, src_basename);

    Buf noextname = BUF_INIT;
    os_path_extname(resolved_path, &noextname, nullptr);

    Buf *pkg_root_src_dir = &package->root_src_dir;
    Buf resolved_root_src_dir = os_path_resolve(&pkg_root_src_dir, 1);

    Buf *namespace_name = buf_create_from_buf(&package->pkg_path);
    if (source_kind == SourceKindNonRoot) {
        assert(buf_starts_with_buf(resolved_path, &resolved_root_src_dir));
        if (buf_len(namespace_name) != 0) {
            buf_append_char(namespace_name, NAMESPACE_SEP_CHAR);
        }
        // The namespace components are obtained from the relative path to the
        // source directory
        if (buf_len(&noextname) > buf_len(&resolved_root_src_dir)) {
            // Skip the trailing separator
            buf_append_mem(namespace_name,
                buf_ptr(&noextname) + buf_len(&resolved_root_src_dir) + 1,
                buf_len(&noextname) - buf_len(&resolved_root_src_dir) - 1);
        }
        buf_replace(namespace_name, ZIG_OS_SEP_CHAR, NAMESPACE_SEP_CHAR);
    }
    Buf *bare_name = buf_alloc();
    os_path_extname(src_basename, bare_name, nullptr);

    RootStruct *root_struct = allocate<RootStruct>(1);
    root_struct->package = package;
    root_struct->source_code = source_code;
    root_struct->line_offsets = tokenization.line_offsets;
    root_struct->path = resolved_path;
    root_struct->di_file = ZigLLVMCreateFile(g->dbuilder, buf_ptr(src_basename), buf_ptr(src_dirname));
    ZigType *import_entry = get_root_container_type(g, buf_ptr(namespace_name), bare_name, root_struct);
    if (source_kind == SourceKindRoot) {
        assert(g->root_import == nullptr);
        g->root_import = import_entry;
    }
    g->import_table.put(resolved_path, import_entry);

    AstNode *root_node = ast_parse(source_code, tokenization.tokens, import_entry, g->err_color);
    assert(root_node != nullptr);
    assert(root_node->type == NodeTypeContainerDecl);
    import_entry->data.structure.decl_node = root_node;
    import_entry->data.structure.decls_scope->base.source_node = root_node;
    if (g->verbose_ast) {
        ast_print(stderr, root_node, 0);
    }

    if (source_kind == SourceKindRoot) {
        // Look for main
        for (size_t decl_i = 0; decl_i < root_node->data.container_decl.decls.length; decl_i += 1) {
            AstNode *top_level_decl = root_node->data.container_decl.decls.at(decl_i);

            if (top_level_decl->type == NodeTypeFnDef) {
                AstNode *proto_node = top_level_decl->data.fn_def.fn_proto;
                assert(proto_node->type == NodeTypeFnProto);
                Buf *proto_name = proto_node->data.fn_proto.name;

                bool is_pub = (proto_node->data.fn_proto.visib_mod == VisibModPub);
                if (is_pub) {
                    if (buf_eql_str(proto_name, "main")) {
                        g->have_pub_main = true;
                    }
                }
            }
        }
    }

    for (size_t decl_i = 0; decl_i < root_node->data.container_decl.decls.length; decl_i += 1) {
        AstNode *top_level_decl = root_node->data.container_decl.decls.at(decl_i);
        scan_decls(g, import_entry->data.structure.decls_scope, top_level_decl);
    }

    TldContainer *tld_container = allocate<TldContainer>(1);
    init_tld(&tld_container->base, TldIdContainer, namespace_name, VisibModPub, root_node, nullptr);
    tld_container->type_entry = import_entry;
    tld_container->decls_scope = import_entry->data.structure.decls_scope;
    g->resolve_queue.append(&tld_container->base);

    return import_entry;
}

void semantic_analyze(CodeGen *g) {
    while (g->resolve_queue_index < g->resolve_queue.length ||
           g->fn_defs_index < g->fn_defs.length)
    {
        for (; g->resolve_queue_index < g->resolve_queue.length; g->resolve_queue_index += 1) {
            Tld *tld = g->resolve_queue.at(g->resolve_queue_index);
            g->trace_err = nullptr;
            AstNode *source_node = nullptr;
            resolve_top_level_decl(g, tld, source_node, false);
        }

        for (; g->fn_defs_index < g->fn_defs.length; g->fn_defs_index += 1) {
            ZigFn *fn_entry = g->fn_defs.at(g->fn_defs_index);
            g->trace_err = nullptr;
            analyze_fn_body(g, fn_entry);
        }
    }

    if (g->errors.length != 0) {
        return;
    }

    // second pass over functions for detecting async
    for (g->fn_defs_index = 0; g->fn_defs_index < g->fn_defs.length; g->fn_defs_index += 1) {
        ZigFn *fn = g->fn_defs.at(g->fn_defs_index);
        g->trace_err = nullptr;
        analyze_fn_async(g, fn, true);
        if (fn->anal_state == FnAnalStateInvalid)
            continue;
        if (fn_is_async(fn) && fn->non_async_node != nullptr) {
            ErrorMsg *msg = add_node_error(g, fn->proto_node,
                buf_sprintf("'%s' cannot be async", buf_ptr(&fn->symbol_name)));
            add_error_note(g, msg, fn->non_async_node,
                buf_sprintf("required to be non-async here"));
            add_async_error_notes(g, msg, fn);
        }
    }
}

ZigType *get_int_type(CodeGen *g, bool is_signed, uint32_t size_in_bits) {
    assert(size_in_bits <= 65535);
    TypeId type_id = {};
    type_id.id = ZigTypeIdInt;
    type_id.data.integer.is_signed = is_signed;
    type_id.data.integer.bit_count = size_in_bits;

    {
        auto entry = g->type_table.maybe_get(type_id);
        if (entry)
            return entry->value;
    }

    ZigType *new_entry = make_int_type(g, is_signed, size_in_bits);
    g->type_table.put(type_id, new_entry);
    return new_entry;
}

bool is_valid_vector_elem_type(ZigType *elem_type) {
    return elem_type->id == ZigTypeIdInt ||
        elem_type->id == ZigTypeIdFloat ||
        elem_type->id == ZigTypeIdBool ||
        get_codegen_ptr_type(elem_type) != nullptr;
}

ZigType *get_vector_type(CodeGen *g, uint32_t len, ZigType *elem_type) {
    assert(is_valid_vector_elem_type(elem_type));

    TypeId type_id = {};
    type_id.id = ZigTypeIdVector;
    type_id.data.vector.len = len;
    type_id.data.vector.elem_type = elem_type;

    {
        auto entry = g->type_table.maybe_get(type_id);
        if (entry)
            return entry->value;
    }

    ZigType *entry = new_type_table_entry(ZigTypeIdVector);
    if ((len != 0) && type_has_bits(elem_type)) {
        // Vectors can only be ints, floats, bools, or pointers. ints (inc. bools) and floats have trivially resolvable
        // llvm type refs. pointers we will use usize instead.
        LLVMTypeRef example_vector_llvm_type;
        if (elem_type->id == ZigTypeIdPointer) {
            example_vector_llvm_type = LLVMVectorType(g->builtin_types.entry_usize->llvm_type, len);
        } else {
            example_vector_llvm_type = LLVMVectorType(elem_type->llvm_type, len);
        }
        assert(example_vector_llvm_type != nullptr);
        entry->size_in_bits = elem_type->size_in_bits * len;
        entry->abi_size = LLVMABISizeOfType(g->target_data_ref, example_vector_llvm_type);
        entry->abi_align = LLVMABIAlignmentOfType(g->target_data_ref, example_vector_llvm_type);
    }
    entry->data.vector.len = len;
    entry->data.vector.elem_type = elem_type;

    buf_resize(&entry->name, 0);
    buf_appendf(&entry->name, "@Vector(%u, %s)", len, buf_ptr(&elem_type->name));

    g->type_table.put(type_id, entry);
    return entry;
}

ZigType **get_c_int_type_ptr(CodeGen *g, CIntType c_int_type) {
    return &g->builtin_types.entry_c_int[c_int_type];
}

ZigType *get_c_int_type(CodeGen *g, CIntType c_int_type) {
    return *get_c_int_type_ptr(g, c_int_type);
}

bool handle_is_ptr(ZigType *type_entry) {
    switch (type_entry->id) {
        case ZigTypeIdInvalid:
        case ZigTypeIdMetaType:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
        case ZigTypeIdOpaque:
             zig_unreachable();
        case ZigTypeIdUnreachable:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdInt:
        case ZigTypeIdFloat:
        case ZigTypeIdPointer:
        case ZigTypeIdErrorSet:
        case ZigTypeIdFn:
        case ZigTypeIdEnum:
        case ZigTypeIdVector:
        case ZigTypeIdAnyFrame:
             return false;
        case ZigTypeIdArray:
        case ZigTypeIdStruct:
        case ZigTypeIdFnFrame:
             return type_has_bits(type_entry);
        case ZigTypeIdErrorUnion:
             return type_has_bits(type_entry->data.error_union.payload_type);
        case ZigTypeIdOptional:
             return type_has_bits(type_entry->data.maybe.child_type) &&
                    !type_is_nonnull_ptr(type_entry->data.maybe.child_type) &&
                    type_entry->data.maybe.child_type->id != ZigTypeIdErrorSet;
        case ZigTypeIdUnion:
             return type_has_bits(type_entry) && type_entry->data.unionation.gen_field_count != 0;

    }
    zig_unreachable();
}

static uint32_t hash_ptr(void *ptr) {
    return (uint32_t)(((uintptr_t)ptr) % UINT32_MAX);
}

static uint32_t hash_size(size_t x) {
    return (uint32_t)(x % UINT32_MAX);
}

uint32_t fn_table_entry_hash(ZigFn* value) {
    return ptr_hash(value);
}

bool fn_table_entry_eql(ZigFn *a, ZigFn *b) {
    return ptr_eq(a, b);
}

uint32_t fn_type_id_hash(FnTypeId *id) {
    uint32_t result = 0;
    result += ((uint32_t)(id->cc)) * (uint32_t)3349388391;
    result += id->is_var_args ? (uint32_t)1931444534 : 0;
    result += hash_ptr(id->return_type);
    result += id->alignment * 0xd3b3f3e2;
    for (size_t i = 0; i < id->param_count; i += 1) {
        FnTypeParamInfo *info = &id->param_info[i];
        result += info->is_noalias ? (uint32_t)892356923 : 0;
        result += hash_ptr(info->type);
    }
    return result;
}

bool fn_type_id_eql(FnTypeId *a, FnTypeId *b) {
    if (a->cc != b->cc ||
        a->return_type != b->return_type ||
        a->is_var_args != b->is_var_args ||
        a->param_count != b->param_count ||
        a->alignment != b->alignment)
    {
        return false;
    }
    for (size_t i = 0; i < a->param_count; i += 1) {
        FnTypeParamInfo *a_param_info = &a->param_info[i];
        FnTypeParamInfo *b_param_info = &b->param_info[i];

        if (a_param_info->type != b_param_info->type ||
            a_param_info->is_noalias != b_param_info->is_noalias)
        {
            return false;
        }
    }
    return true;
}

static uint32_t hash_const_val_error_set(ConstExprValue *const_val) {
    assert(const_val->data.x_err_set != nullptr);
    return const_val->data.x_err_set->value ^ 2630160122;
}

static uint32_t hash_const_val_ptr(ConstExprValue *const_val) {
    uint32_t hash_val = 0;
    switch (const_val->data.x_ptr.mut) {
        case ConstPtrMutRuntimeVar:
            hash_val += (uint32_t)3500721036;
            break;
        case ConstPtrMutComptimeConst:
            hash_val += (uint32_t)4214318515;
            break;
        case ConstPtrMutInfer:
        case ConstPtrMutComptimeVar:
            hash_val += (uint32_t)1103195694;
            break;
    }
    switch (const_val->data.x_ptr.special) {
        case ConstPtrSpecialInvalid:
            zig_unreachable();
        case ConstPtrSpecialRef:
            hash_val += (uint32_t)2478261866;
            hash_val += hash_ptr(const_val->data.x_ptr.data.ref.pointee);
            return hash_val;
        case ConstPtrSpecialBaseArray:
            hash_val += (uint32_t)1764906839;
            hash_val += hash_ptr(const_val->data.x_ptr.data.base_array.array_val);
            hash_val += hash_size(const_val->data.x_ptr.data.base_array.elem_index);
            hash_val += const_val->data.x_ptr.data.base_array.is_cstr ? 1297263887 : 200363492;
            return hash_val;
        case ConstPtrSpecialBaseStruct:
            hash_val += (uint32_t)3518317043;
            hash_val += hash_ptr(const_val->data.x_ptr.data.base_struct.struct_val);
            hash_val += hash_size(const_val->data.x_ptr.data.base_struct.field_index);
            return hash_val;
        case ConstPtrSpecialBaseErrorUnionCode:
            hash_val += (uint32_t)2994743799;
            hash_val += hash_ptr(const_val->data.x_ptr.data.base_err_union_code.err_union_val);
            return hash_val;
        case ConstPtrSpecialBaseErrorUnionPayload:
            hash_val += (uint32_t)3456080131;
            hash_val += hash_ptr(const_val->data.x_ptr.data.base_err_union_payload.err_union_val);
            return hash_val;
        case ConstPtrSpecialBaseOptionalPayload:
            hash_val += (uint32_t)3163140517;
            hash_val += hash_ptr(const_val->data.x_ptr.data.base_optional_payload.optional_val);
            return hash_val;
        case ConstPtrSpecialHardCodedAddr:
            hash_val += (uint32_t)4048518294;
            hash_val += hash_size(const_val->data.x_ptr.data.hard_coded_addr.addr);
            return hash_val;
        case ConstPtrSpecialDiscard:
            hash_val += 2010123162;
            return hash_val;
        case ConstPtrSpecialFunction:
            hash_val += (uint32_t)2590901619;
            hash_val += hash_ptr(const_val->data.x_ptr.data.fn.fn_entry);
            return hash_val;
        case ConstPtrSpecialNull:
            hash_val += (uint32_t)1486246455;
            return hash_val;
    }
    zig_unreachable();
}

static uint32_t hash_const_val(ConstExprValue *const_val) {
    assert(const_val->special == ConstValSpecialStatic);
    switch (const_val->type->id) {
        case ZigTypeIdOpaque:
            zig_unreachable();
        case ZigTypeIdBool:
            return const_val->data.x_bool ? (uint32_t)127863866 : (uint32_t)215080464;
        case ZigTypeIdMetaType:
            return hash_ptr(const_val->data.x_type);
        case ZigTypeIdVoid:
            return (uint32_t)4149439618;
        case ZigTypeIdInt:
        case ZigTypeIdComptimeInt:
            {
                uint32_t result = 1331471175;
                for (size_t i = 0; i < const_val->data.x_bigint.digit_count; i += 1) {
                    uint64_t digit = bigint_ptr(&const_val->data.x_bigint)[i];
                    result ^= ((uint32_t)(digit >> 32)) ^ (uint32_t)(result);
                }
                return result;
            }
        case ZigTypeIdEnumLiteral:
            return buf_hash(const_val->data.x_enum_literal) * 2691276464;
        case ZigTypeIdEnum:
            {
                uint32_t result = 31643936;
                for (size_t i = 0; i < const_val->data.x_enum_tag.digit_count; i += 1) {
                    uint64_t digit = bigint_ptr(&const_val->data.x_enum_tag)[i];
                    result ^= ((uint32_t)(digit >> 32)) ^ (uint32_t)(result);
                }
                return result;
            }
        case ZigTypeIdFloat:
            switch (const_val->type->data.floating.bit_count) {
                case 16:
                    {
                        uint16_t result;
                        static_assert(sizeof(result) == sizeof(const_val->data.x_f16), "");
                        memcpy(&result, &const_val->data.x_f16, sizeof(result));
                        return result * 65537u;
                    }
                case 32:
                    {
                        uint32_t result;
                        memcpy(&result, &const_val->data.x_f32, 4);
                        return result ^ 4084870010;
                    }
                case 64:
                    {
                        uint32_t ints[2];
                        memcpy(&ints[0], &const_val->data.x_f64, 8);
                        return ints[0] ^ ints[1] ^ 0x22ed43c6;
                    }
                case 128:
                    {
                        uint32_t ints[4];
                        memcpy(&ints[0], &const_val->data.x_f128, 16);
                        return ints[0] ^ ints[1] ^ ints[2] ^ ints[3] ^ 0xb5ffef27;
                    }
                default:
                    zig_unreachable();
            }
        case ZigTypeIdComptimeFloat:
            {
                float128_t f128 = bigfloat_to_f128(&const_val->data.x_bigfloat);
                uint32_t ints[4];
                memcpy(&ints[0], &f128, 16);
                return ints[0] ^ ints[1] ^ ints[2] ^ ints[3] ^ 0xed8b3dfb;
            }
        case ZigTypeIdArgTuple:
            return (uint32_t)const_val->data.x_arg_tuple.start_index * (uint32_t)281907309 +
                (uint32_t)const_val->data.x_arg_tuple.end_index * (uint32_t)2290442768;
        case ZigTypeIdFn:
            assert(const_val->data.x_ptr.mut == ConstPtrMutComptimeConst);
            assert(const_val->data.x_ptr.special == ConstPtrSpecialFunction);
            return 3677364617 ^ hash_ptr(const_val->data.x_ptr.data.fn.fn_entry);
        case ZigTypeIdPointer:
            return hash_const_val_ptr(const_val);
        case ZigTypeIdUndefined:
            return 162837799;
        case ZigTypeIdNull:
            return 844854567;
        case ZigTypeIdArray:
            // TODO better hashing algorithm
            return 1166190605;
        case ZigTypeIdStruct:
            // TODO better hashing algorithm
            return 1532530855;
        case ZigTypeIdUnion:
            // TODO better hashing algorithm
            return 2709806591;
        case ZigTypeIdOptional:
            if (get_codegen_ptr_type(const_val->type) != nullptr) {
                return hash_const_val_ptr(const_val) * 1992916303;
            } else if (const_val->type->data.maybe.child_type->id == ZigTypeIdErrorSet) {
                return hash_const_val_error_set(const_val) * 3147031929;
            } else {
                if (const_val->data.x_optional) {
                    return hash_const_val(const_val->data.x_optional) * 1992916303;
                } else {
                    return 4016830364;
                }
            }
        case ZigTypeIdErrorUnion:
            // TODO better hashing algorithm
            return 3415065496;
        case ZigTypeIdErrorSet:
            return hash_const_val_error_set(const_val);
        case ZigTypeIdVector:
            // TODO better hashing algorithm
            return 3647867726;
        case ZigTypeIdFnFrame:
            // TODO better hashing algorithm
            return 675741936;
        case ZigTypeIdAnyFrame:
            // TODO better hashing algorithm
            return 3747294894;
        case ZigTypeIdBoundFn:
        case ZigTypeIdInvalid:
        case ZigTypeIdUnreachable:
            zig_unreachable();
    }
    zig_unreachable();
}

uint32_t generic_fn_type_id_hash(GenericFnTypeId *id) {
    uint32_t result = 0;
    result += hash_ptr(id->fn_entry);
    for (size_t i = 0; i < id->param_count; i += 1) {
        ConstExprValue *generic_param = &id->params[i];
        if (generic_param->special != ConstValSpecialRuntime) {
            result += hash_const_val(generic_param);
            result += hash_ptr(generic_param->type);
        }
    }
    return result;
}

bool generic_fn_type_id_eql(GenericFnTypeId *a, GenericFnTypeId *b) {
    assert(a->fn_entry);
    if (a->fn_entry != b->fn_entry) return false;
    if (a->param_count != b->param_count) return false;
    for (size_t i = 0; i < a->param_count; i += 1) {
        ConstExprValue *a_val = &a->params[i];
        ConstExprValue *b_val = &b->params[i];
        if (a_val->type != b_val->type) return false;
        if (a_val->special != ConstValSpecialRuntime && b_val->special != ConstValSpecialRuntime) {
            assert(a_val->special == ConstValSpecialStatic);
            assert(b_val->special == ConstValSpecialStatic);
            if (!const_values_equal(a->codegen, a_val, b_val)) {
                return false;
            }
        } else {
            assert(a_val->special == ConstValSpecialRuntime && b_val->special == ConstValSpecialRuntime);
        }
    }
    return true;
}

static bool can_mutate_comptime_var_state(ConstExprValue *value) {
    assert(value != nullptr);
    switch (value->type->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdMetaType:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdUnreachable:
        case ZigTypeIdInt:
        case ZigTypeIdVector:
        case ZigTypeIdFloat:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdBoundFn:
        case ZigTypeIdFn:
        case ZigTypeIdOpaque:
        case ZigTypeIdErrorSet:
        case ZigTypeIdEnum:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            return false;

        case ZigTypeIdPointer:
            return value->data.x_ptr.mut == ConstPtrMutComptimeVar;

        case ZigTypeIdArray:
            if (value->type->data.array.len == 0)
                return false;
            switch (value->data.x_array.special) {
                case ConstArraySpecialUndef:
                case ConstArraySpecialBuf:
                    return false;
                case ConstArraySpecialNone:
                    for (uint32_t i = 0; i < value->type->data.array.len; i += 1) {
                        if (can_mutate_comptime_var_state(&value->data.x_array.data.s_none.elements[i]))
                            return true;
                    }
                    return false;
            }
            zig_unreachable();
        case ZigTypeIdStruct:
            for (uint32_t i = 0; i < value->type->data.structure.src_field_count; i += 1) {
                if (can_mutate_comptime_var_state(&value->data.x_struct.fields[i]))
                    return true;
            }
            return false;

        case ZigTypeIdOptional:
            if (get_codegen_ptr_type(value->type) != nullptr)
                return value->data.x_ptr.mut == ConstPtrMutComptimeVar;
            if (value->data.x_optional == nullptr)
                return false;
            return can_mutate_comptime_var_state(value->data.x_optional);

        case ZigTypeIdErrorUnion:
            if (value->data.x_err_union.error_set->data.x_err_set != nullptr)
                return false;
            assert(value->data.x_err_union.payload != nullptr);
            return can_mutate_comptime_var_state(value->data.x_err_union.payload);

        case ZigTypeIdUnion:
            return can_mutate_comptime_var_state(value->data.x_union.payload);

        case ZigTypeIdArgTuple:
            zig_panic("TODO var args at comptime is currently not supported");
    }
    zig_unreachable();
}

static bool return_type_is_cacheable(ZigType *return_type) {
    switch (return_type->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdMetaType:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdUnreachable:
        case ZigTypeIdInt:
        case ZigTypeIdFloat:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdBoundFn:
        case ZigTypeIdFn:
        case ZigTypeIdOpaque:
        case ZigTypeIdErrorSet:
        case ZigTypeIdEnum:
        case ZigTypeIdPointer:
        case ZigTypeIdVector:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            return true;

        case ZigTypeIdArray:
        case ZigTypeIdStruct:
        case ZigTypeIdUnion:
            return false;

        case ZigTypeIdOptional:
            return return_type_is_cacheable(return_type->data.maybe.child_type);

        case ZigTypeIdErrorUnion:
            return return_type_is_cacheable(return_type->data.error_union.payload_type);

        case ZigTypeIdArgTuple:
            zig_panic("TODO var args at comptime is currently not supported");
    }
    zig_unreachable();
}

bool fn_eval_cacheable(Scope *scope, ZigType *return_type) {
    if (!return_type_is_cacheable(return_type))
        return false;
    while (scope) {
        if (scope->id == ScopeIdVarDecl) {
            ScopeVarDecl *var_scope = (ScopeVarDecl *)scope;
            if (type_is_invalid(var_scope->var->var_type))
                return false;
            if (var_scope->var->const_value->special == ConstValSpecialUndef)
                return false;
            if (can_mutate_comptime_var_state(var_scope->var->const_value))
                return false;
        } else if (scope->id == ScopeIdFnDef) {
            return true;
        } else {
            zig_unreachable();
        }

        scope = scope->parent;
    }
    zig_unreachable();
}

uint32_t fn_eval_hash(Scope* scope) {
    uint32_t result = 0;
    while (scope) {
        if (scope->id == ScopeIdVarDecl) {
            ScopeVarDecl *var_scope = (ScopeVarDecl *)scope;
            result += hash_const_val(var_scope->var->const_value);
        } else if (scope->id == ScopeIdFnDef) {
            ScopeFnDef *fn_scope = (ScopeFnDef *)scope;
            result += hash_ptr(fn_scope->fn_entry);
            return result;
        } else {
            zig_unreachable();
        }

        scope = scope->parent;
    }
    zig_unreachable();
}

bool fn_eval_eql(Scope *a, Scope *b) {
    assert(a->codegen != nullptr);
    assert(b->codegen != nullptr);
    while (a && b) {
        if (a->id != b->id)
            return false;

        if (a->id == ScopeIdVarDecl) {
            ScopeVarDecl *a_var_scope = (ScopeVarDecl *)a;
            ScopeVarDecl *b_var_scope = (ScopeVarDecl *)b;
            if (a_var_scope->var->var_type != b_var_scope->var->var_type)
                return false;
            if (a_var_scope->var->var_type == a_var_scope->var->const_value->type &&
                b_var_scope->var->var_type == b_var_scope->var->const_value->type)
            {
                if (!const_values_equal(a->codegen, a_var_scope->var->const_value, b_var_scope->var->const_value))
                    return false;
            } else {
                zig_panic("TODO comptime ptr reinterpret for fn_eval_eql");
            }
        } else if (a->id == ScopeIdFnDef) {
            ScopeFnDef *a_fn_scope = (ScopeFnDef *)a;
            ScopeFnDef *b_fn_scope = (ScopeFnDef *)b;
            if (a_fn_scope->fn_entry != b_fn_scope->fn_entry)
                return false;

            return true;
        } else {
            zig_unreachable();
        }

        a = a->parent;
        b = b->parent;
    }
    return false;
}

// Whether the type has bits at runtime.
bool type_has_bits(ZigType *type_entry) {
    assert(type_entry != nullptr);
    assert(!type_is_invalid(type_entry));
    assert(type_is_resolved(type_entry, ResolveStatusZeroBitsKnown));
    return type_entry->abi_size != 0;
}

// Whether you can infer the value based solely on the type.
OnePossibleValue type_has_one_possible_value(CodeGen *g, ZigType *type_entry) {
    assert(type_entry != nullptr);

    if (type_entry->one_possible_value != OnePossibleValueInvalid)
        return type_entry->one_possible_value;

    Error err;
    if ((err = type_resolve(g, type_entry, ResolveStatusZeroBitsKnown)))
        return OnePossibleValueInvalid;
    switch (type_entry->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdOpaque:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdMetaType:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
        case ZigTypeIdOptional:
        case ZigTypeIdFn:
        case ZigTypeIdBool:
        case ZigTypeIdFloat:
        case ZigTypeIdErrorUnion:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            return OnePossibleValueNo;
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdVoid:
        case ZigTypeIdUnreachable:
            return OnePossibleValueYes;
        case ZigTypeIdArray:
            if (type_entry->data.array.len == 0)
                return OnePossibleValueYes;
            return type_has_one_possible_value(g, type_entry->data.array.child_type);
        case ZigTypeIdStruct:
            for (size_t i = 0; i < type_entry->data.structure.src_field_count; i += 1) {
                TypeStructField *field = &type_entry->data.structure.fields[i];
                OnePossibleValue opv = (field->type_entry != nullptr) ?
                    type_has_one_possible_value(g, field->type_entry) :
                    type_val_resolve_has_one_possible_value(g, field->type_val);
                switch (opv) {
                    case OnePossibleValueInvalid:
                        return OnePossibleValueInvalid;
                    case OnePossibleValueNo:
                        return OnePossibleValueNo;
                    case OnePossibleValueYes:
                        continue;
                }
            }
            return OnePossibleValueYes;
        case ZigTypeIdErrorSet:
        case ZigTypeIdEnum:
        case ZigTypeIdInt:
        case ZigTypeIdVector:
            return type_has_bits(type_entry) ? OnePossibleValueNo : OnePossibleValueYes;
        case ZigTypeIdPointer: {
            ZigType *elem_type = type_entry->data.pointer.child_type;
            // If the recursive function call asks, then we are not one possible value.
            type_entry->one_possible_value = OnePossibleValueNo;
            // Now update it to be the value of the recursive call.
            type_entry->one_possible_value = type_has_one_possible_value(g, elem_type);
            return type_entry->one_possible_value;
        }
        case ZigTypeIdUnion:
            if (type_entry->data.unionation.src_field_count > 1)
                return OnePossibleValueNo;
            TypeUnionField *only_field = &type_entry->data.unionation.fields[0];
            if (only_field->type_entry != nullptr) {
                return type_has_one_possible_value(g, only_field->type_entry);
            }
            return type_val_resolve_has_one_possible_value(g, only_field->type_val);
    }
    zig_unreachable();
}

ReqCompTime type_requires_comptime(CodeGen *g, ZigType *ty) {
    Error err;
    switch (ty->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdMetaType:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
            return ReqCompTimeYes;
        case ZigTypeIdArray:
            return type_requires_comptime(g, ty->data.array.child_type);
        case ZigTypeIdStruct:
            if (ty->data.structure.resolve_loop_flag_zero_bits) {
                // Does a struct which contains a pointer field to itself require comptime? No.
                return ReqCompTimeNo;
            }
            if ((err = type_resolve(g, ty, ResolveStatusZeroBitsKnown)))
                return ReqCompTimeInvalid;
            return ty->data.structure.requires_comptime ? ReqCompTimeYes : ReqCompTimeNo;
        case ZigTypeIdUnion:
            if (ty->data.unionation.resolve_loop_flag_zero_bits) {
                // Does a union which contains a pointer field to itself require comptime? No.
                return ReqCompTimeNo;
            }
            if ((err = type_resolve(g, ty, ResolveStatusZeroBitsKnown)))
                return ReqCompTimeInvalid;
            return ty->data.unionation.requires_comptime ? ReqCompTimeYes : ReqCompTimeNo;
        case ZigTypeIdOptional:
            return type_requires_comptime(g, ty->data.maybe.child_type);
        case ZigTypeIdErrorUnion:
            return type_requires_comptime(g, ty->data.error_union.payload_type);
        case ZigTypeIdPointer:
            if (ty->data.pointer.child_type->id == ZigTypeIdOpaque) {
                return ReqCompTimeNo;
            } else {
                return type_requires_comptime(g, ty->data.pointer.child_type);
            }
        case ZigTypeIdFn:
            return ty->data.fn.is_generic ? ReqCompTimeYes : ReqCompTimeNo;
        case ZigTypeIdOpaque:
        case ZigTypeIdEnum:
        case ZigTypeIdErrorSet:
        case ZigTypeIdBool:
        case ZigTypeIdInt:
        case ZigTypeIdVector:
        case ZigTypeIdFloat:
        case ZigTypeIdVoid:
        case ZigTypeIdUnreachable:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            return ReqCompTimeNo;
    }
    zig_unreachable();
}

void init_const_str_lit(CodeGen *g, ConstExprValue *const_val, Buf *str) {
    auto entry = g->string_literals_table.maybe_get(str);
    if (entry != nullptr) {
        memcpy(const_val, entry->value, sizeof(ConstExprValue));
        return;
    }

    const_val->special = ConstValSpecialStatic;
    const_val->type = get_array_type(g, g->builtin_types.entry_u8, buf_len(str));
    const_val->data.x_array.special = ConstArraySpecialBuf;
    const_val->data.x_array.data.s_buf = str;

    g->string_literals_table.put(str, const_val);
}

ConstExprValue *create_const_str_lit(CodeGen *g, Buf *str) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_str_lit(g, const_val, str);
    return const_val;
}

void init_const_c_str_lit(CodeGen *g, ConstExprValue *const_val, Buf *str) {
    // first we build the underlying array
    size_t len_with_null = buf_len(str) + 1;
    ConstExprValue *array_val = create_const_vals(1);
    array_val->special = ConstValSpecialStatic;
    array_val->type = get_array_type(g, g->builtin_types.entry_u8, len_with_null);
    // TODO buf optimization
    array_val->data.x_array.data.s_none.elements = create_const_vals(len_with_null);
    for (size_t i = 0; i < buf_len(str); i += 1) {
        ConstExprValue *this_char = &array_val->data.x_array.data.s_none.elements[i];
        this_char->special = ConstValSpecialStatic;
        this_char->type = g->builtin_types.entry_u8;
        bigint_init_unsigned(&this_char->data.x_bigint, (uint8_t)buf_ptr(str)[i]);
    }
    ConstExprValue *null_char = &array_val->data.x_array.data.s_none.elements[len_with_null - 1];
    null_char->special = ConstValSpecialStatic;
    null_char->type = g->builtin_types.entry_u8;
    bigint_init_unsigned(&null_char->data.x_bigint, 0);

    // then make the pointer point to it
    const_val->special = ConstValSpecialStatic;
    // TODO make this `[*]null u8` instead of `[*]u8`
    const_val->type = get_pointer_to_type_extra(g, g->builtin_types.entry_u8, true, false,
            PtrLenUnknown, 0, 0, 0, false);
    const_val->data.x_ptr.special = ConstPtrSpecialBaseArray;
    const_val->data.x_ptr.data.base_array.array_val = array_val;
    const_val->data.x_ptr.data.base_array.elem_index = 0;
    const_val->data.x_ptr.data.base_array.is_cstr = true;
}
ConstExprValue *create_const_c_str_lit(CodeGen *g, Buf *str) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_c_str_lit(g, const_val, str);
    return const_val;
}

void init_const_bigint(ConstExprValue *const_val, ZigType *type, const BigInt *bigint) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = type;
    bigint_init_bigint(&const_val->data.x_bigint, bigint);
}

ConstExprValue *create_const_bigint(ZigType *type, const BigInt *bigint) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_bigint(const_val, type, bigint);
    return const_val;
}


void init_const_unsigned_negative(ConstExprValue *const_val, ZigType *type, uint64_t x, bool negative) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = type;
    bigint_init_unsigned(&const_val->data.x_bigint, x);
    const_val->data.x_bigint.is_negative = negative;
}

ConstExprValue *create_const_unsigned_negative(ZigType *type, uint64_t x, bool negative) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_unsigned_negative(const_val, type, x, negative);
    return const_val;
}

void init_const_usize(CodeGen *g, ConstExprValue *const_val, uint64_t x) {
    return init_const_unsigned_negative(const_val, g->builtin_types.entry_usize, x, false);
}

ConstExprValue *create_const_usize(CodeGen *g, uint64_t x) {
    return create_const_unsigned_negative(g->builtin_types.entry_usize, x, false);
}

void init_const_signed(ConstExprValue *const_val, ZigType *type, int64_t x) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = type;
    bigint_init_signed(&const_val->data.x_bigint, x);
}

ConstExprValue *create_const_signed(ZigType *type, int64_t x) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_signed(const_val, type, x);
    return const_val;
}

void init_const_float(ConstExprValue *const_val, ZigType *type, double value) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = type;
    if (type->id == ZigTypeIdComptimeFloat) {
        bigfloat_init_64(&const_val->data.x_bigfloat, value);
    } else if (type->id == ZigTypeIdFloat) {
        switch (type->data.floating.bit_count) {
            case 16:
                const_val->data.x_f16 = zig_double_to_f16(value);
                break;
            case 32:
                const_val->data.x_f32 = value;
                break;
            case 64:
                const_val->data.x_f64 = value;
                break;
            case 128:
                // if we need this, we should add a function that accepts a float128_t param
                zig_unreachable();
            default:
                zig_unreachable();
        }
    } else {
        zig_unreachable();
    }
}

ConstExprValue *create_const_float(ZigType *type, double value) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_float(const_val, type, value);
    return const_val;
}

void init_const_enum(ConstExprValue *const_val, ZigType *type, const BigInt *tag) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = type;
    bigint_init_bigint(&const_val->data.x_enum_tag, tag);
}

ConstExprValue *create_const_enum(ZigType *type, const BigInt *tag) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_enum(const_val, type, tag);
    return const_val;
}


void init_const_bool(CodeGen *g, ConstExprValue *const_val, bool value) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = g->builtin_types.entry_bool;
    const_val->data.x_bool = value;
}

ConstExprValue *create_const_bool(CodeGen *g, bool value) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_bool(g, const_val, value);
    return const_val;
}

void init_const_runtime(ConstExprValue *const_val, ZigType *type) {
    const_val->special = ConstValSpecialRuntime;
    const_val->type = type;
}

ConstExprValue *create_const_runtime(ZigType *type) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_runtime(const_val, type);
    return const_val;
}

void init_const_type(CodeGen *g, ConstExprValue *const_val, ZigType *type_value) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = g->builtin_types.entry_type;
    const_val->data.x_type = type_value;
}

ConstExprValue *create_const_type(CodeGen *g, ZigType *type_value) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_type(g, const_val, type_value);
    return const_val;
}

void init_const_slice(CodeGen *g, ConstExprValue *const_val, ConstExprValue *array_val,
        size_t start, size_t len, bool is_const)
{
    assert(array_val->type->id == ZigTypeIdArray);

    ZigType *ptr_type = get_pointer_to_type_extra(g, array_val->type->data.array.child_type,
            is_const, false, PtrLenUnknown, 0, 0, 0, false);

    const_val->special = ConstValSpecialStatic;
    const_val->type = get_slice_type(g, ptr_type);
    const_val->data.x_struct.fields = create_const_vals(2);

    init_const_ptr_array(g, &const_val->data.x_struct.fields[slice_ptr_index], array_val, start, is_const,
            PtrLenUnknown);
    init_const_usize(g, &const_val->data.x_struct.fields[slice_len_index], len);
}

ConstExprValue *create_const_slice(CodeGen *g, ConstExprValue *array_val, size_t start, size_t len, bool is_const) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_slice(g, const_val, array_val, start, len, is_const);
    return const_val;
}

void init_const_ptr_array(CodeGen *g, ConstExprValue *const_val, ConstExprValue *array_val,
        size_t elem_index, bool is_const, PtrLen ptr_len)
{
    assert(array_val->type->id == ZigTypeIdArray);
    ZigType *child_type = array_val->type->data.array.child_type;

    const_val->special = ConstValSpecialStatic;
    const_val->type = get_pointer_to_type_extra(g, child_type, is_const, false,
            ptr_len, 0, 0, 0, false);
    const_val->data.x_ptr.special = ConstPtrSpecialBaseArray;
    const_val->data.x_ptr.data.base_array.array_val = array_val;
    const_val->data.x_ptr.data.base_array.elem_index = elem_index;
}

ConstExprValue *create_const_ptr_array(CodeGen *g, ConstExprValue *array_val, size_t elem_index, bool is_const,
        PtrLen ptr_len)
{
    ConstExprValue *const_val = create_const_vals(1);
    init_const_ptr_array(g, const_val, array_val, elem_index, is_const, ptr_len);
    return const_val;
}

void init_const_ptr_ref(CodeGen *g, ConstExprValue *const_val, ConstExprValue *pointee_val, bool is_const) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = get_pointer_to_type(g, pointee_val->type, is_const);
    const_val->data.x_ptr.special = ConstPtrSpecialRef;
    const_val->data.x_ptr.data.ref.pointee = pointee_val;
}

ConstExprValue *create_const_ptr_ref(CodeGen *g, ConstExprValue *pointee_val, bool is_const) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_ptr_ref(g, const_val, pointee_val, is_const);
    return const_val;
}

void init_const_ptr_hard_coded_addr(CodeGen *g, ConstExprValue *const_val, ZigType *pointee_type,
        size_t addr, bool is_const)
{
    const_val->special = ConstValSpecialStatic;
    const_val->type = get_pointer_to_type(g, pointee_type, is_const);
    const_val->data.x_ptr.special = ConstPtrSpecialHardCodedAddr;
    const_val->data.x_ptr.data.hard_coded_addr.addr = addr;
}

ConstExprValue *create_const_ptr_hard_coded_addr(CodeGen *g, ZigType *pointee_type,
        size_t addr, bool is_const)
{
    ConstExprValue *const_val = create_const_vals(1);
    init_const_ptr_hard_coded_addr(g, const_val, pointee_type, addr, is_const);
    return const_val;
}

void init_const_arg_tuple(CodeGen *g, ConstExprValue *const_val, size_t arg_index_start, size_t arg_index_end) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = g->builtin_types.entry_arg_tuple;
    const_val->data.x_arg_tuple.start_index = arg_index_start;
    const_val->data.x_arg_tuple.end_index = arg_index_end;
}

ConstExprValue *create_const_arg_tuple(CodeGen *g, size_t arg_index_start, size_t arg_index_end) {
    ConstExprValue *const_val = create_const_vals(1);
    init_const_arg_tuple(g, const_val, arg_index_start, arg_index_end);
    return const_val;
}


ConstExprValue *create_const_vals(size_t count) {
    ConstGlobalRefs *global_refs = allocate<ConstGlobalRefs>(count, "ConstGlobalRefs");
    ConstExprValue *vals = allocate<ConstExprValue>(count, "ConstExprValue");
    for (size_t i = 0; i < count; i += 1) {
        vals[i].global_refs = &global_refs[i];
    }
    return vals;
}

static ZigType *get_async_fn_type(CodeGen *g, ZigType *orig_fn_type) {
    if (orig_fn_type->data.fn.fn_type_id.cc == CallingConventionAsync)
        return orig_fn_type;

    ZigType *fn_type = allocate_nonzero<ZigType>(1);
    *fn_type = *orig_fn_type;
    fn_type->data.fn.fn_type_id.cc = CallingConventionAsync;
    fn_type->llvm_type = nullptr;
    fn_type->llvm_di_type = nullptr;

    return fn_type;
}

// Traverse up to the very top ExprScope, which has children.
// We have just arrived at the top from a child. That child,
// and its next siblings, do not need to be marked. But the previous
// siblings do.
//      x + (await y)
// vs
//      (await y) + x
static void mark_suspension_point(Scope *scope) {
    ScopeExpr *child_expr_scope = (scope->id == ScopeIdExpr) ? reinterpret_cast<ScopeExpr *>(scope) : nullptr;
    bool looking_for_exprs = true;
    for (;;) {
        scope = scope->parent;
        switch (scope->id) {
            case ScopeIdDeferExpr:
            case ScopeIdDecls:
            case ScopeIdFnDef:
            case ScopeIdCompTime:
            case ScopeIdCImport:
            case ScopeIdSuspend:
            case ScopeIdTypeOf:
                return;
            case ScopeIdVarDecl:
            case ScopeIdDefer:
            case ScopeIdBlock:
                looking_for_exprs = false;
                continue;
            case ScopeIdRuntime:
                continue;
            case ScopeIdLoop: {
                ScopeLoop *loop_scope = reinterpret_cast<ScopeLoop *>(scope);
                if (loop_scope->spill_scope != nullptr) {
                    loop_scope->spill_scope->need_spill = MemoizedBoolTrue;
                }
                looking_for_exprs = false;
                continue;
            }
            case ScopeIdExpr: {
                if (!looking_for_exprs) {
                    // Now we're only looking for a block, to see if it's in a loop (see the case ScopeIdBlock)
                    continue;
                }
                ScopeExpr *parent_expr_scope = reinterpret_cast<ScopeExpr *>(scope);
                if (child_expr_scope != nullptr) {
                    for (size_t i = 0; parent_expr_scope->children_ptr[i] != child_expr_scope; i += 1) {
                        assert(i < parent_expr_scope->children_len);
                        parent_expr_scope->children_ptr[i]->need_spill = MemoizedBoolTrue;
                    }
                }
                parent_expr_scope->need_spill = MemoizedBoolTrue;
                child_expr_scope = parent_expr_scope;
                continue;
            }
        }
    }
}

static bool scope_needs_spill(Scope *scope) {
    ScopeExpr *scope_expr = find_expr_scope(scope);
    if (scope_expr == nullptr) return false;

    switch (scope_expr->need_spill) {
        case MemoizedBoolUnknown:
            if (scope_needs_spill(scope_expr->base.parent)) {
                scope_expr->need_spill = MemoizedBoolTrue;
                return true;
            } else {
                scope_expr->need_spill = MemoizedBoolFalse;
                return false;
            }
        case MemoizedBoolFalse:
            return false;
        case MemoizedBoolTrue:
            return true;
    }
    zig_unreachable();
}

static Error resolve_async_frame(CodeGen *g, ZigType *frame_type) {
    Error err;

    if (frame_type->data.frame.locals_struct != nullptr)
        return ErrorNone;

    ZigFn *fn = frame_type->data.frame.fn;
    assert(!fn->type_entry->data.fn.is_generic);

    if (frame_type->data.frame.resolve_loop_type != nullptr) {
        if (!frame_type->data.frame.reported_loop_err) {
            add_node_error(g, fn->proto_node,
                    buf_sprintf("'%s' depends on itself", buf_ptr(&frame_type->name)));
        }
        return ErrorSemanticAnalyzeFail;
    }

    switch (fn->anal_state) {
        case FnAnalStateInvalid:
            return ErrorSemanticAnalyzeFail;
        case FnAnalStateComplete:
            break;
        case FnAnalStateReady:
            analyze_fn_body(g, fn);
            if (fn->anal_state == FnAnalStateInvalid)
                return ErrorSemanticAnalyzeFail;
            break;
        case FnAnalStateProbing: {
            add_node_error(g, fn->proto_node,
                    buf_sprintf("cannot resolve '%s': function not fully analyzed yet",
                        buf_ptr(&frame_type->name)));
            return ErrorSemanticAnalyzeFail;
        }
    }
    analyze_fn_async(g, fn, false);
    if (fn->anal_state == FnAnalStateInvalid)
        return ErrorSemanticAnalyzeFail;

    if (!fn_is_async(fn)) {
        ZigType *fn_type = fn->type_entry;
        FnTypeId *fn_type_id = &fn_type->data.fn.fn_type_id;
        ZigType *ptr_return_type = get_pointer_to_type(g, fn_type_id->return_type, false);

        // label (grep this): [fn_frame_struct_layout]
        ZigList<SrcField> fields = {};

        fields.append({"@fn_ptr", g->builtin_types.entry_usize, 0});
        fields.append({"@resume_index", g->builtin_types.entry_usize, 0});
        fields.append({"@awaiter", g->builtin_types.entry_usize, 0});

        fields.append({"@result_ptr_callee", ptr_return_type, 0});
        fields.append({"@result_ptr_awaiter", ptr_return_type, 0});
        fields.append({"@result", fn_type_id->return_type, 0});

        if (codegen_fn_has_err_ret_tracing_arg(g, fn_type_id->return_type)) {
            ZigType *ptr_to_stack_trace_type = get_pointer_to_type(g, get_stack_trace_type(g), false);
            fields.append({"@ptr_stack_trace_callee", ptr_to_stack_trace_type, 0});
            fields.append({"@ptr_stack_trace_awaiter", ptr_to_stack_trace_type, 0});

            fields.append({"@stack_trace", get_stack_trace_type(g), 0});
            fields.append({"@instruction_addresses",
                    get_array_type(g, g->builtin_types.entry_usize, stack_trace_ptr_count), 0});
        }

        frame_type->data.frame.locals_struct = get_struct_type(g, buf_ptr(&frame_type->name),
                fields.items, fields.length, target_fn_align(g->zig_target));
        frame_type->abi_size = frame_type->data.frame.locals_struct->abi_size;
        frame_type->abi_align = frame_type->data.frame.locals_struct->abi_align;
        frame_type->size_in_bits = frame_type->data.frame.locals_struct->size_in_bits;

        return ErrorNone;
    }

    ZigType *fn_type = get_async_fn_type(g, fn->type_entry);

    if (fn->analyzed_executable.need_err_code_spill) {
        IrInstructionAllocaGen *alloca_gen = allocate<IrInstructionAllocaGen>(1);
        alloca_gen->base.id = IrInstructionIdAllocaGen;
        alloca_gen->base.source_node = fn->proto_node;
        alloca_gen->base.scope = fn->child_scope;
        alloca_gen->base.value.type = get_pointer_to_type(g, g->builtin_types.entry_global_error_set, false);
        alloca_gen->base.ref_count = 1;
        alloca_gen->name_hint = "";
        fn->alloca_gen_list.append(alloca_gen);
        fn->err_code_spill = &alloca_gen->base;
    }

    ZigType *largest_call_frame_type = nullptr;
    // Later we'll change this to be largest_call_frame_type instead of void.
    IrInstruction *all_calls_alloca = ir_create_alloca(g, &fn->fndef_scope->base, fn->body_node,
            fn, g->builtin_types.entry_void, "@async_call_frame");

    for (size_t i = 0; i < fn->call_list.length; i += 1) {
        IrInstructionCallGen *call = fn->call_list.at(i);
        if (call->new_stack != nullptr) {
            // don't need to allocate a frame for this
            continue;
        }
        ZigFn *callee = call->fn_entry;
        if (callee == nullptr) {
            add_node_error(g, call->base.source_node,
                buf_sprintf("function is not comptime-known; @asyncCall required"));
            return ErrorSemanticAnalyzeFail;
        }
        if (callee->body_node == nullptr) {
            continue;
        }
        if (callee->anal_state == FnAnalStateProbing) {
            ErrorMsg *msg = add_node_error(g, fn->proto_node,
                buf_sprintf("unable to determine async function frame of '%s'", buf_ptr(&fn->symbol_name)));
            g->trace_err = add_error_note(g, msg, call->base.source_node,
                buf_sprintf("analysis of function '%s' depends on the frame", buf_ptr(&callee->symbol_name)));
            return ErrorSemanticAnalyzeFail;
        }

        ZigType *callee_frame_type = get_fn_frame_type(g, callee);
        frame_type->data.frame.resolve_loop_type = callee_frame_type;
        frame_type->data.frame.resolve_loop_src_node = call->base.source_node;

        analyze_fn_body(g, callee);
        if (callee->anal_state == FnAnalStateInvalid) {
            frame_type->data.frame.locals_struct = g->builtin_types.entry_invalid;
            return ErrorSemanticAnalyzeFail;
        }
        analyze_fn_async(g, callee, true);
        if (callee->inferred_async_node == inferred_async_checking) {
            assert(g->errors.length != 0);
            frame_type->data.frame.locals_struct = g->builtin_types.entry_invalid;
            return ErrorSemanticAnalyzeFail;
        }
        if (!fn_is_async(callee))
            continue;

        mark_suspension_point(call->base.scope);

        if ((err = type_resolve(g, callee_frame_type, ResolveStatusSizeKnown))) {
            return err;
        }
        if (largest_call_frame_type == nullptr ||
            callee_frame_type->abi_size > largest_call_frame_type->abi_size)
        {
            largest_call_frame_type = callee_frame_type;
        }

        call->frame_result_loc = all_calls_alloca;
    }
    if (largest_call_frame_type != nullptr) {
        all_calls_alloca->value.type = get_pointer_to_type(g, largest_call_frame_type, false);
    }

    // Since this frame is async, an await might represent a suspend point, and
    // therefore need to spill. It also needs to mark expr scopes as having to spill.
    // For example: foo() + await z
    // The funtion call result of foo() must be spilled.
    for (size_t i = 0; i < fn->await_list.length; i += 1) {
        IrInstructionAwaitGen *await = fn->await_list.at(i);
        // TODO If this is a noasync await, it doesn't suspend
        // https://github.com/ziglang/zig/issues/3157
        if (await->base.value.special != ConstValSpecialRuntime) {
            // Known at comptime. No spill, no suspend.
            continue;
        }
        if (await->target_fn != nullptr) {
            // we might not need to suspend
            analyze_fn_async(g, await->target_fn, false);
            if (await->target_fn->anal_state == FnAnalStateInvalid) {
                frame_type->data.frame.locals_struct = g->builtin_types.entry_invalid;
                return ErrorSemanticAnalyzeFail;
            }
            if (!fn_is_async(await->target_fn)) {
                // This await does not represent a suspend point. No spill needed,
                // and no need to mark ExprScope.
                continue;
            }
        }
        // This await is a suspend point, but it might not need a spill.
        // We do need to mark the ExprScope as having a suspend point in it.
        mark_suspension_point(await->base.scope);

        if (await->result_loc != nullptr) {
            // If there's a result location, that is the spill
            continue;
        }
        if (await->base.ref_count == 0)
            continue;
        if (!type_has_bits(await->base.value.type))
            continue;
        await->result_loc = ir_create_alloca(g, await->base.scope, await->base.source_node, fn,
                await->base.value.type, "");
    }
    for (size_t block_i = 0; block_i < fn->analyzed_executable.basic_block_list.length; block_i += 1) {
        IrBasicBlock *block = fn->analyzed_executable.basic_block_list.at(block_i);
        for (size_t instr_i = 0; instr_i < block->instruction_list.length; instr_i += 1) {
            IrInstruction *instruction = block->instruction_list.at(instr_i);
            if (instruction->id == IrInstructionIdSuspendFinish) {
                mark_suspension_point(instruction->scope);
            }
        }
    }
    // Now that we've marked all the expr scopes that have to spill, we go over the instructions
    // and spill the relevant ones.
    for (size_t block_i = 0; block_i < fn->analyzed_executable.basic_block_list.length; block_i += 1) {
        IrBasicBlock *block = fn->analyzed_executable.basic_block_list.at(block_i);
        for (size_t instr_i = 0; instr_i < block->instruction_list.length; instr_i += 1) {
            IrInstruction *instruction = block->instruction_list.at(instr_i);
            if (instruction->id == IrInstructionIdAwaitGen ||
                instruction->id == IrInstructionIdVarPtr ||
                instruction->id == IrInstructionIdDeclRef ||
                instruction->id == IrInstructionIdAllocaGen)
            {
                // This instruction does its own spilling specially, or otherwise doesn't need it.
                continue;
            }
            if (instruction->value.special != ConstValSpecialRuntime)
                continue;
            if (instruction->ref_count == 0)
                continue;
            if (!type_has_bits(instruction->value.type))
                continue;
            if (scope_needs_spill(instruction->scope)) {
                instruction->spill = ir_create_alloca(g, instruction->scope, instruction->source_node,
                        fn, instruction->value.type, "");
            }
        }
    }

    FnTypeId *fn_type_id = &fn_type->data.fn.fn_type_id;
    ZigType *ptr_return_type = get_pointer_to_type(g, fn_type_id->return_type, false);

    // label (grep this): [fn_frame_struct_layout]
    ZigList<SrcField> fields = {};

    fields.append({"@fn_ptr", fn_type, 0});
    fields.append({"@resume_index", g->builtin_types.entry_usize, 0});
    fields.append({"@awaiter", g->builtin_types.entry_usize, 0});

    fields.append({"@result_ptr_callee", ptr_return_type, 0});
    fields.append({"@result_ptr_awaiter", ptr_return_type, 0});
    fields.append({"@result", fn_type_id->return_type, 0});

    if (codegen_fn_has_err_ret_tracing_arg(g, fn_type_id->return_type)) {
        ZigType *ptr_stack_trace_type = get_pointer_to_type(g, get_stack_trace_type(g), false);
        fields.append({"@ptr_stack_trace_callee", ptr_stack_trace_type, 0});
        fields.append({"@ptr_stack_trace_awaiter", ptr_stack_trace_type, 0});
    }

    for (size_t arg_i = 0; arg_i < fn_type_id->param_count; arg_i += 1) {
        FnTypeParamInfo *param_info = &fn_type_id->param_info[arg_i];
        AstNode *param_decl_node = get_param_decl_node(fn, arg_i);
        Buf *param_name;
        bool is_var_args = param_decl_node && param_decl_node->data.param_decl.is_var_args;
        if (param_decl_node && !is_var_args) {
            param_name = param_decl_node->data.param_decl.name;
        } else {
            param_name = buf_sprintf("@arg%" ZIG_PRI_usize, arg_i);
        }
        ZigType *param_type = param_info->type;
        if ((err = type_resolve(g, param_type, ResolveStatusSizeKnown))) {
            return err;
        }

        fields.append({buf_ptr(param_name), param_type, 0});
    }

    if (codegen_fn_has_err_ret_tracing_stack(g, fn, true)) {
        fields.append({"@stack_trace", get_stack_trace_type(g), 0});
        fields.append({"@instruction_addresses",
                get_array_type(g, g->builtin_types.entry_usize, stack_trace_ptr_count), 0});
    }

    for (size_t alloca_i = 0; alloca_i < fn->alloca_gen_list.length; alloca_i += 1) {
        IrInstructionAllocaGen *instruction = fn->alloca_gen_list.at(alloca_i);
        instruction->field_index = SIZE_MAX;
        ZigType *ptr_type = instruction->base.value.type;
        assert(ptr_type->id == ZigTypeIdPointer);
        ZigType *child_type = ptr_type->data.pointer.child_type;
        if (!type_has_bits(child_type))
            continue;
        if (instruction->base.ref_count == 0)
            continue;
        if (instruction->base.value.special != ConstValSpecialRuntime) {
            if (const_ptr_pointee(nullptr, g, &instruction->base.value, nullptr)->special !=
                    ConstValSpecialRuntime)
            {
                continue;
            }
        }

        frame_type->data.frame.resolve_loop_type = child_type;
        frame_type->data.frame.resolve_loop_src_node = instruction->base.source_node;
        if ((err = type_resolve(g, child_type, ResolveStatusSizeKnown))) {
            return err;
        }

        const char *name;
        if (*instruction->name_hint == 0) {
            name = buf_ptr(buf_sprintf("@local%" ZIG_PRI_usize, alloca_i));
        } else {
            name = buf_ptr(buf_sprintf("%s.%" ZIG_PRI_usize, instruction->name_hint, alloca_i));
        }
        instruction->field_index = fields.length;

        fields.append({name, child_type, instruction->align});
    }


    frame_type->data.frame.locals_struct = get_struct_type(g, buf_ptr(&frame_type->name),
            fields.items, fields.length, target_fn_align(g->zig_target));
    frame_type->abi_size = frame_type->data.frame.locals_struct->abi_size;
    frame_type->abi_align = frame_type->data.frame.locals_struct->abi_align;
    frame_type->size_in_bits = frame_type->data.frame.locals_struct->size_in_bits;

    if (g->largest_frame_fn == nullptr || frame_type->abi_size > g->largest_frame_fn->frame_type->abi_size) {
        g->largest_frame_fn = fn;
    }

    return ErrorNone;
}

static Error resolve_pointer_zero_bits(CodeGen *g, ZigType *ty) {
    Error err;

    if (ty->abi_size != SIZE_MAX)
        return ErrorNone;

    if (ty->data.pointer.resolve_loop_flag_zero_bits) {
        ty->abi_size = g->builtin_types.entry_usize->abi_size;
        ty->size_in_bits = g->builtin_types.entry_usize->size_in_bits;
        ty->abi_align = g->builtin_types.entry_usize->abi_align;
        return ErrorNone;
    }
    ty->data.pointer.resolve_loop_flag_zero_bits = true;

    ZigType *elem_type = ty->data.pointer.child_type;

    if ((err = type_resolve(g, elem_type, ResolveStatusZeroBitsKnown)))
        return err;

    if (type_has_bits(elem_type)) {
        ty->abi_size = g->builtin_types.entry_usize->abi_size;
        ty->size_in_bits = g->builtin_types.entry_usize->size_in_bits;
        ty->abi_align = g->builtin_types.entry_usize->abi_align;
    } else {
        ty->abi_size = 0;
        ty->size_in_bits = 0;
        ty->abi_align = 0;
    }
    return ErrorNone;
}

Error type_resolve(CodeGen *g, ZigType *ty, ResolveStatus status) {
    if (type_is_invalid(ty))
        return ErrorSemanticAnalyzeFail;
    switch (status) {
        case ResolveStatusUnstarted:
            return ErrorNone;
        case ResolveStatusInvalid:
            zig_unreachable();
        case ResolveStatusZeroBitsKnown:
            switch (ty->id) {
                case ZigTypeIdStruct:
                    return resolve_struct_zero_bits(g, ty);
                case ZigTypeIdEnum:
                    return resolve_enum_zero_bits(g, ty);
                case ZigTypeIdUnion:
                    return resolve_union_zero_bits(g, ty);
                case ZigTypeIdPointer:
                    return resolve_pointer_zero_bits(g, ty);
                default:
                    return ErrorNone;
            }
        case ResolveStatusAlignmentKnown:
            switch (ty->id) {
                case ZigTypeIdStruct:
                    return resolve_struct_alignment(g, ty);
                case ZigTypeIdEnum:
                    return resolve_enum_zero_bits(g, ty);
                case ZigTypeIdUnion:
                    return resolve_union_alignment(g, ty);
                case ZigTypeIdFnFrame:
                    return resolve_async_frame(g, ty);
                case ZigTypeIdPointer:
                    return resolve_pointer_zero_bits(g, ty);
                default:
                    return ErrorNone;
            }
        case ResolveStatusSizeKnown:
            switch (ty->id) {
                case ZigTypeIdStruct:
                    return resolve_struct_type(g, ty);
                case ZigTypeIdEnum:
                    return resolve_enum_zero_bits(g, ty);
                case ZigTypeIdUnion:
                    return resolve_union_type(g, ty);
                case ZigTypeIdFnFrame:
                    return resolve_async_frame(g, ty);
                case ZigTypeIdPointer:
                    return resolve_pointer_zero_bits(g, ty);
                default:
                    return ErrorNone;
            }
        case ResolveStatusLLVMFwdDecl:
        case ResolveStatusLLVMFull:
            resolve_llvm_types(g, ty, status);
            return ErrorNone;
    }
    zig_unreachable();
}

bool ir_get_var_is_comptime(ZigVar *var) {
    // The is_comptime field can be left null, which means not comptime.
    if (var->is_comptime == nullptr)
        return false;
    // When the is_comptime field references an instruction that has to get analyzed, this
    // is the value.
    if (var->is_comptime->child != nullptr) {
        assert(var->is_comptime->child->value.type->id == ZigTypeIdBool);
        return var->is_comptime->child->value.data.x_bool;
    }
    // As an optimization, is_comptime values which are constant are allowed
    // to be omitted from analysis. In this case, there is no child instruction
    // and we simply look at the unanalyzed const parent instruction.
    assert(var->is_comptime->value.type->id == ZigTypeIdBool);
    return var->is_comptime->value.data.x_bool;
}

bool const_values_equal_ptr(ConstExprValue *a, ConstExprValue *b) {
    if (a->data.x_ptr.special != b->data.x_ptr.special)
        return false;
    if (a->data.x_ptr.mut != b->data.x_ptr.mut)
        return false;
    switch (a->data.x_ptr.special) {
        case ConstPtrSpecialInvalid:
            zig_unreachable();
        case ConstPtrSpecialRef:
            if (a->data.x_ptr.data.ref.pointee != b->data.x_ptr.data.ref.pointee)
                return false;
            return true;
        case ConstPtrSpecialBaseArray:
            if (a->data.x_ptr.data.base_array.array_val != b->data.x_ptr.data.base_array.array_val &&
                a->data.x_ptr.data.base_array.array_val->global_refs !=
                b->data.x_ptr.data.base_array.array_val->global_refs)
            {
                return false;
            }
            if (a->data.x_ptr.data.base_array.elem_index != b->data.x_ptr.data.base_array.elem_index)
                return false;
            if (a->data.x_ptr.data.base_array.is_cstr != b->data.x_ptr.data.base_array.is_cstr)
                return false;
            return true;
        case ConstPtrSpecialBaseStruct:
            if (a->data.x_ptr.data.base_struct.struct_val != b->data.x_ptr.data.base_struct.struct_val &&
                a->data.x_ptr.data.base_struct.struct_val->global_refs !=
                b->data.x_ptr.data.base_struct.struct_val->global_refs)
            {
                return false;
            }
            if (a->data.x_ptr.data.base_struct.field_index != b->data.x_ptr.data.base_struct.field_index)
                return false;
            return true;
        case ConstPtrSpecialBaseErrorUnionCode:
            if (a->data.x_ptr.data.base_err_union_code.err_union_val !=
                b->data.x_ptr.data.base_err_union_code.err_union_val &&
                a->data.x_ptr.data.base_err_union_code.err_union_val->global_refs !=
                b->data.x_ptr.data.base_err_union_code.err_union_val->global_refs)
            {
                return false;
            }
            return true;
        case ConstPtrSpecialBaseErrorUnionPayload:
            if (a->data.x_ptr.data.base_err_union_payload.err_union_val !=
                b->data.x_ptr.data.base_err_union_payload.err_union_val &&
                a->data.x_ptr.data.base_err_union_payload.err_union_val->global_refs !=
                b->data.x_ptr.data.base_err_union_payload.err_union_val->global_refs)
            {
                return false;
            }
            return true;
        case ConstPtrSpecialBaseOptionalPayload:
            if (a->data.x_ptr.data.base_optional_payload.optional_val !=
                b->data.x_ptr.data.base_optional_payload.optional_val &&
                a->data.x_ptr.data.base_optional_payload.optional_val->global_refs !=
                b->data.x_ptr.data.base_optional_payload.optional_val->global_refs)
            {
                return false;
            }
            return true;
        case ConstPtrSpecialHardCodedAddr:
            if (a->data.x_ptr.data.hard_coded_addr.addr != b->data.x_ptr.data.hard_coded_addr.addr)
                return false;
            return true;
        case ConstPtrSpecialDiscard:
            return true;
        case ConstPtrSpecialFunction:
            return a->data.x_ptr.data.fn.fn_entry == b->data.x_ptr.data.fn.fn_entry;
        case ConstPtrSpecialNull:
            return true;
    }
    zig_unreachable();
}

static bool const_values_equal_array(CodeGen *g, ConstExprValue *a, ConstExprValue *b, size_t len) {
    assert(a->data.x_array.special != ConstArraySpecialUndef);
    assert(b->data.x_array.special != ConstArraySpecialUndef);
    if (a->data.x_array.special == ConstArraySpecialBuf &&
        b->data.x_array.special == ConstArraySpecialBuf)
    {
        return buf_eql_buf(a->data.x_array.data.s_buf, b->data.x_array.data.s_buf);
    }
    expand_undef_array(g, a);
    expand_undef_array(g, b);

    ConstExprValue *a_elems = a->data.x_array.data.s_none.elements;
    ConstExprValue *b_elems = b->data.x_array.data.s_none.elements;

    for (size_t i = 0; i < len; i += 1) {
        if (!const_values_equal(g, &a_elems[i], &b_elems[i]))
            return false;
    }

    return true;
}

bool const_values_equal(CodeGen *g, ConstExprValue *a, ConstExprValue *b) {
    assert(a->type->id == b->type->id);
    assert(a->special == ConstValSpecialStatic);
    assert(b->special == ConstValSpecialStatic);
    switch (a->type->id) {
        case ZigTypeIdOpaque:
            zig_unreachable();
        case ZigTypeIdEnum:
            return bigint_cmp(&a->data.x_enum_tag, &b->data.x_enum_tag) == CmpEQ;
        case ZigTypeIdUnion: {
            ConstUnionValue *union1 = &a->data.x_union;
            ConstUnionValue *union2 = &b->data.x_union;

            if (bigint_cmp(&union1->tag, &union2->tag) == CmpEQ) {
                TypeUnionField *field = find_union_field_by_tag(a->type, &union1->tag);
                assert(field != nullptr);
                if (!type_has_bits(field->type_entry))
                    return true;
                assert(find_union_field_by_tag(a->type, &union2->tag) != nullptr);
                return const_values_equal(g, union1->payload, union2->payload);
            }
            return false;
        }
        case ZigTypeIdMetaType:
            return a->data.x_type == b->data.x_type;
        case ZigTypeIdVoid:
            return true;
        case ZigTypeIdErrorSet:
            return a->data.x_err_set->value == b->data.x_err_set->value;
        case ZigTypeIdBool:
            return a->data.x_bool == b->data.x_bool;
        case ZigTypeIdFloat:
            assert(a->type->data.floating.bit_count == b->type->data.floating.bit_count);
            switch (a->type->data.floating.bit_count) {
                case 16:
                    return f16_eq(a->data.x_f16, b->data.x_f16);
                case 32:
                    return a->data.x_f32 == b->data.x_f32;
                case 64:
                    return a->data.x_f64 == b->data.x_f64;
                case 128:
                    return f128M_eq(&a->data.x_f128, &b->data.x_f128);
                default:
                    zig_unreachable();
            }
        case ZigTypeIdComptimeFloat:
            return bigfloat_cmp(&a->data.x_bigfloat, &b->data.x_bigfloat) == CmpEQ;
        case ZigTypeIdInt:
        case ZigTypeIdComptimeInt:
            return bigint_cmp(&a->data.x_bigint, &b->data.x_bigint) == CmpEQ;
        case ZigTypeIdEnumLiteral:
            return buf_eql_buf(a->data.x_enum_literal, b->data.x_enum_literal);
        case ZigTypeIdPointer:
        case ZigTypeIdFn:
            return const_values_equal_ptr(a, b);
        case ZigTypeIdVector:
            assert(a->type->data.vector.len == b->type->data.vector.len);
            return const_values_equal_array(g, a, b, a->type->data.vector.len);
        case ZigTypeIdArray: {
            assert(a->type->data.array.len == b->type->data.array.len);
            return const_values_equal_array(g, a, b, a->type->data.array.len);
        }
        case ZigTypeIdStruct:
            for (size_t i = 0; i < a->type->data.structure.src_field_count; i += 1) {
                ConstExprValue *field_a = &a->data.x_struct.fields[i];
                ConstExprValue *field_b = &b->data.x_struct.fields[i];
                if (!const_values_equal(g, field_a, field_b))
                    return false;
            }
            return true;
        case ZigTypeIdFnFrame:
            zig_panic("TODO");
        case ZigTypeIdAnyFrame:
            zig_panic("TODO");
        case ZigTypeIdUndefined:
            zig_panic("TODO");
        case ZigTypeIdNull:
            zig_panic("TODO");
        case ZigTypeIdOptional:
            if (get_codegen_ptr_type(a->type) != nullptr)
                return const_values_equal_ptr(a, b);
            if (a->data.x_optional == nullptr || b->data.x_optional == nullptr) {
                return (a->data.x_optional == nullptr && b->data.x_optional == nullptr);
            } else {
                return const_values_equal(g, a->data.x_optional, b->data.x_optional);
            }
        case ZigTypeIdErrorUnion:
            zig_panic("TODO");
        case ZigTypeIdArgTuple:
            return a->data.x_arg_tuple.start_index == b->data.x_arg_tuple.start_index &&
                   a->data.x_arg_tuple.end_index == b->data.x_arg_tuple.end_index;
        case ZigTypeIdBoundFn:
        case ZigTypeIdInvalid:
        case ZigTypeIdUnreachable:
            zig_unreachable();
    }
    zig_unreachable();
}

void eval_min_max_value_int(CodeGen *g, ZigType *int_type, BigInt *bigint, bool is_max) {
    assert(int_type->id == ZigTypeIdInt);
    if (int_type->data.integral.bit_count == 0) {
        bigint_init_unsigned(bigint, 0);
        return;
    }
    if (is_max) {
        // is_signed=true   (1 << (bit_count - 1)) - 1
        // is_signed=false  (1 << (bit_count - 0)) - 1
        BigInt one = {0};
        bigint_init_unsigned(&one, 1);

        size_t shift_amt = int_type->data.integral.bit_count - (int_type->data.integral.is_signed ? 1 : 0);
        BigInt bit_count_bi = {0};
        bigint_init_unsigned(&bit_count_bi, shift_amt);

        BigInt shifted_bi = {0};
        bigint_shl(&shifted_bi, &one, &bit_count_bi);

        bigint_sub(bigint, &shifted_bi, &one);
    } else if (int_type->data.integral.is_signed) {
        // - (1 << (bit_count - 1))
        BigInt one = {0};
        bigint_init_unsigned(&one, 1);

        BigInt bit_count_bi = {0};
        bigint_init_unsigned(&bit_count_bi, int_type->data.integral.bit_count - 1);

        BigInt shifted_bi = {0};
        bigint_shl(&shifted_bi, &one, &bit_count_bi);

        bigint_negate(bigint, &shifted_bi);
    } else {
        bigint_init_unsigned(bigint, 0);
    }
}

void eval_min_max_value(CodeGen *g, ZigType *type_entry, ConstExprValue *const_val, bool is_max) {
    if (type_entry->id == ZigTypeIdInt) {
        const_val->special = ConstValSpecialStatic;
        eval_min_max_value_int(g, type_entry, &const_val->data.x_bigint, is_max);
    } else if (type_entry->id == ZigTypeIdBool) {
        const_val->special = ConstValSpecialStatic;
        const_val->data.x_bool = is_max;
    } else if (type_entry->id == ZigTypeIdVoid) {
        // nothing to do
    } else {
        zig_unreachable();
    }
}

static void render_const_val_ptr(CodeGen *g, Buf *buf, ConstExprValue *const_val, ZigType *type_entry) {
    if (type_entry->id == ZigTypeIdPointer && type_entry->data.pointer.child_type->id == ZigTypeIdOpaque) {
        buf_append_buf(buf, &type_entry->name);
        return;
    }

    switch (const_val->data.x_ptr.special) {
        case ConstPtrSpecialInvalid:
            zig_unreachable();
        case ConstPtrSpecialRef:
        case ConstPtrSpecialBaseStruct:
        case ConstPtrSpecialBaseErrorUnionCode:
        case ConstPtrSpecialBaseErrorUnionPayload:
        case ConstPtrSpecialBaseOptionalPayload:
            buf_appendf(buf, "*");
            // TODO we need a source node for const_ptr_pointee because it can generate compile errors
            render_const_value(g, buf, const_ptr_pointee(nullptr, g, const_val, nullptr));
            return;
        case ConstPtrSpecialBaseArray:
            if (const_val->data.x_ptr.data.base_array.is_cstr) {
                buf_appendf(buf, "*(c str lit)");
                return;
            } else {
                buf_appendf(buf, "*");
                // TODO we need a source node for const_ptr_pointee because it can generate compile errors
                render_const_value(g, buf, const_ptr_pointee(nullptr, g, const_val, nullptr));
                return;
            }
        case ConstPtrSpecialHardCodedAddr:
            buf_appendf(buf, "(%s)(%" ZIG_PRI_x64 ")", buf_ptr(&type_entry->name),
                    const_val->data.x_ptr.data.hard_coded_addr.addr);
            return;
        case ConstPtrSpecialDiscard:
            buf_append_str(buf, "*_");
            return;
        case ConstPtrSpecialFunction:
            {
                ZigFn *fn_entry = const_val->data.x_ptr.data.fn.fn_entry;
                buf_appendf(buf, "@ptrCast(%s, %s)", buf_ptr(&const_val->type->name), buf_ptr(&fn_entry->symbol_name));
                return;
            }
        case ConstPtrSpecialNull:
            buf_append_str(buf, "null");
            return;
    }
    zig_unreachable();
}

static void render_const_val_err_set(CodeGen *g, Buf *buf, ConstExprValue *const_val, ZigType *type_entry) {
    if (const_val->data.x_err_set == nullptr) {
        buf_append_str(buf, "null");
    } else {
        buf_appendf(buf, "%s.%s", buf_ptr(&type_entry->name), buf_ptr(&const_val->data.x_err_set->name));
    }
}

static void render_const_val_array(CodeGen *g, Buf *buf, Buf *type_name, ConstExprValue *const_val, uint64_t start, uint64_t len) {
    ConstArrayValue *array = &const_val->data.x_array;
    switch (array->special) {
        case ConstArraySpecialUndef:
            buf_append_str(buf, "undefined");
            return;
        case ConstArraySpecialBuf: {
            Buf *array_buf = array->data.s_buf;
            const char *base = &buf_ptr(array_buf)[start];
            assert(start + len <= buf_len(array_buf));

            buf_append_char(buf, '"');
            for (size_t i = 0; i < len; i += 1) {
                uint8_t c = base[i];
                if (c == '"') {
                    buf_append_str(buf, "\\\"");
                } else {
                    buf_append_char(buf, c);
                }
            }
            buf_append_char(buf, '"');
            return;
        }
        case ConstArraySpecialNone: {
            ConstExprValue *base = &array->data.s_none.elements[start];
            assert(start + len <= const_val->type->data.array.len);

            buf_appendf(buf, "%s{", buf_ptr(type_name));
            for (uint64_t i = 0; i < len; i += 1) {
                if (i != 0) buf_appendf(buf, ",");
                render_const_value(g, buf, &base[i]);
            }
            buf_appendf(buf, "}");
            return;
        }
    }
    zig_unreachable();
}

void render_const_value(CodeGen *g, Buf *buf, ConstExprValue *const_val) {
    switch (const_val->special) {
        case ConstValSpecialRuntime:
            buf_appendf(buf, "(runtime value)");
            return;
        case ConstValSpecialLazy:
            buf_appendf(buf, "(lazy value)");
            return;
        case ConstValSpecialUndef:
            buf_appendf(buf, "undefined");
            return;
        case ConstValSpecialStatic:
            break;
    }
    assert(const_val->type);

    ZigType *type_entry = const_val->type;
    switch (type_entry->id) {
        case ZigTypeIdOpaque:
            zig_unreachable();
        case ZigTypeIdInvalid:
            buf_appendf(buf, "(invalid)");
            return;
        case ZigTypeIdVoid:
            buf_appendf(buf, "{}");
            return;
        case ZigTypeIdComptimeFloat:
            bigfloat_append_buf(buf, &const_val->data.x_bigfloat);
            return;
        case ZigTypeIdFloat:
            switch (type_entry->data.floating.bit_count) {
                case 16:
                    buf_appendf(buf, "%f", zig_f16_to_double(const_val->data.x_f16));
                    return;
                case 32:
                    buf_appendf(buf, "%f", const_val->data.x_f32);
                    return;
                case 64:
                    buf_appendf(buf, "%f", const_val->data.x_f64);
                    return;
                case 128:
                    {
                        const size_t extra_len = 100;
                        size_t old_len = buf_len(buf);
                        buf_resize(buf, old_len + extra_len);
                        float64_t f64_value = f128M_to_f64(&const_val->data.x_f128);
                        double double_value;
                        memcpy(&double_value, &f64_value, sizeof(double));
                        // TODO actual f128 printing to decimal
                        int len = snprintf(buf_ptr(buf) + old_len, extra_len, "%f", double_value);
                        assert(len > 0);
                        buf_resize(buf, old_len + len);
                        return;
                    }
                default:
                    zig_unreachable();
            }
        case ZigTypeIdComptimeInt:
        case ZigTypeIdInt:
            bigint_append_buf(buf, &const_val->data.x_bigint, 10);
            return;
        case ZigTypeIdEnumLiteral:
            buf_append_buf(buf, const_val->data.x_enum_literal);
            return;
        case ZigTypeIdMetaType:
            buf_appendf(buf, "%s", buf_ptr(&const_val->data.x_type->name));
            return;
        case ZigTypeIdUnreachable:
            buf_appendf(buf, "unreachable");
            return;
        case ZigTypeIdBool:
            {
                const char *value = const_val->data.x_bool ? "true" : "false";
                buf_appendf(buf, "%s", value);
                return;
            }
        case ZigTypeIdFn:
            {
                assert(const_val->data.x_ptr.mut == ConstPtrMutComptimeConst);
                assert(const_val->data.x_ptr.special == ConstPtrSpecialFunction);
                ZigFn *fn_entry = const_val->data.x_ptr.data.fn.fn_entry;
                buf_appendf(buf, "%s", buf_ptr(&fn_entry->symbol_name));
                return;
            }
        case ZigTypeIdPointer:
            return render_const_val_ptr(g, buf, const_val, type_entry);
        case ZigTypeIdArray: {
            uint64_t len = type_entry->data.array.len;
            render_const_val_array(g, buf, &type_entry->name, const_val, 0, len);
            return;
        }
        case ZigTypeIdVector: {
            uint32_t len = type_entry->data.vector.len;
            render_const_val_array(g, buf, &type_entry->name, const_val, 0, len);
            return;
        }
        case ZigTypeIdNull:
            {
                buf_appendf(buf, "null");
                return;
            }
        case ZigTypeIdUndefined:
            {
                buf_appendf(buf, "undefined");
                return;
            }
        case ZigTypeIdOptional:
            {
                if (get_codegen_ptr_type(const_val->type) != nullptr)
                    return render_const_val_ptr(g, buf, const_val, type_entry->data.maybe.child_type);
                if (type_entry->data.maybe.child_type->id == ZigTypeIdErrorSet)
                    return render_const_val_err_set(g, buf, const_val, type_entry->data.maybe.child_type);
                if (const_val->data.x_optional) {
                    render_const_value(g, buf, const_val->data.x_optional);
                } else {
                    buf_appendf(buf, "null");
                }
                return;
            }
        case ZigTypeIdBoundFn:
            {
                ZigFn *fn_entry = const_val->data.x_bound_fn.fn;
                buf_appendf(buf, "(bound fn %s)", buf_ptr(&fn_entry->symbol_name));
                return;
            }
        case ZigTypeIdStruct:
            {
                if (is_slice(type_entry)) {
                    ConstExprValue *len_val = &const_val->data.x_struct.fields[slice_len_index];
                    size_t len = bigint_as_usize(&len_val->data.x_bigint);

                    ConstExprValue *ptr_val = &const_val->data.x_struct.fields[slice_ptr_index];
                    if (ptr_val->special == ConstValSpecialUndef) {
                        assert(len == 0);
                        buf_appendf(buf, "((%s)(undefined))[0..0]", buf_ptr(&type_entry->name));
                        return;
                    }
                    assert(ptr_val->data.x_ptr.special == ConstPtrSpecialBaseArray);
                    ConstExprValue *array = ptr_val->data.x_ptr.data.base_array.array_val;
                    size_t start = ptr_val->data.x_ptr.data.base_array.elem_index;

                    render_const_val_array(g, buf, &type_entry->name, array, start, len);
                } else {
                    buf_appendf(buf, "(struct %s constant)", buf_ptr(&type_entry->name));
                }
                return;
            }
        case ZigTypeIdEnum:
            {
                TypeEnumField *field = find_enum_field_by_tag(type_entry, &const_val->data.x_enum_tag);
                buf_appendf(buf, "%s.%s", buf_ptr(&type_entry->name), buf_ptr(field->name));
                return;
            }
        case ZigTypeIdErrorUnion:
            {
                buf_appendf(buf, "%s(", buf_ptr(&type_entry->name));
                ErrorTableEntry *err_set = const_val->data.x_err_union.error_set->data.x_err_set;
                if (err_set == nullptr) {
                    render_const_value(g, buf, const_val->data.x_err_union.payload);
                } else {
                    buf_appendf(buf, "%s.%s", buf_ptr(&type_entry->data.error_union.err_set_type->name),
                            buf_ptr(&err_set->name));
                }
                buf_appendf(buf, ")");
                return;
            }
        case ZigTypeIdUnion:
            {
                const BigInt *tag = &const_val->data.x_union.tag;
                TypeUnionField *field = find_union_field_by_tag(type_entry, tag);
                buf_appendf(buf, "%s { .%s = ", buf_ptr(&type_entry->name), buf_ptr(field->name));
                render_const_value(g, buf, const_val->data.x_union.payload);
                buf_append_str(buf, "}");
                return;
            }
        case ZigTypeIdErrorSet:
            return render_const_val_err_set(g, buf, const_val, type_entry);
        case ZigTypeIdArgTuple:
            {
                buf_appendf(buf, "(args value)");
                return;
            }
        case ZigTypeIdFnFrame:
            buf_appendf(buf, "(TODO: async function frame value)");
            return;

        case ZigTypeIdAnyFrame:
            buf_appendf(buf, "(TODO: anyframe value)");
            return;

    }
    zig_unreachable();
}

ZigType *make_int_type(CodeGen *g, bool is_signed, uint32_t size_in_bits) {
    assert(size_in_bits <= 65535);
    ZigType *entry = new_type_table_entry(ZigTypeIdInt);

    entry->size_in_bits = size_in_bits;
    if (size_in_bits != 0) {
        entry->llvm_type = LLVMIntType(size_in_bits);
        entry->abi_size = LLVMABISizeOfType(g->target_data_ref, entry->llvm_type);
        entry->abi_align = LLVMABIAlignmentOfType(g->target_data_ref, entry->llvm_type);

        if (size_in_bits >= 128 && entry->abi_align < 16) {
            // Override the incorrect alignment reported by LLVM. Clang does this as well.
            // On x86_64 there are some instructions like CMPXCHG16B which require this.
            // On all targets, integers 128 bits and above have ABI alignment of 16.
            // However for some targets, LLVM incorrectly reports this as 8.
            // See: https://github.com/ziglang/zig/issues/2987
            entry->abi_align = 16;
        }
    }

    const char u_or_i = is_signed ? 'i' : 'u';
    buf_resize(&entry->name, 0);
    buf_appendf(&entry->name, "%c%" PRIu32, u_or_i, size_in_bits);

    entry->data.integral.is_signed = is_signed;
    entry->data.integral.bit_count = size_in_bits;
    return entry;
}

uint32_t type_id_hash(TypeId x) {
    switch (x.id) {
        case ZigTypeIdInvalid:
        case ZigTypeIdOpaque:
        case ZigTypeIdMetaType:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdUnreachable:
        case ZigTypeIdFloat:
        case ZigTypeIdStruct:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdOptional:
        case ZigTypeIdErrorSet:
        case ZigTypeIdEnum:
        case ZigTypeIdUnion:
        case ZigTypeIdFn:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            zig_unreachable();
        case ZigTypeIdErrorUnion:
            return hash_ptr(x.data.error_union.err_set_type) ^ hash_ptr(x.data.error_union.payload_type);
        case ZigTypeIdPointer:
            return hash_ptr(x.data.pointer.child_type) +
                ((x.data.pointer.ptr_len == PtrLenSingle) ? (uint32_t)1120226602 : (uint32_t)3200913342) +
                (x.data.pointer.is_const ? (uint32_t)2749109194 : (uint32_t)4047371087) +
                (x.data.pointer.is_volatile ? (uint32_t)536730450 : (uint32_t)1685612214) +
                (x.data.pointer.allow_zero ? (uint32_t)3324284834 : (uint32_t)3584904923) +
                (((uint32_t)x.data.pointer.alignment) ^ (uint32_t)0x777fbe0e) +
                (((uint32_t)x.data.pointer.bit_offset_in_host) ^ (uint32_t)2639019452) +
                (((uint32_t)x.data.pointer.host_int_bytes) ^ (uint32_t)529908881);
        case ZigTypeIdArray:
            return hash_ptr(x.data.array.child_type) +
                ((uint32_t)x.data.array.size ^ (uint32_t)2122979968);
        case ZigTypeIdInt:
            return (x.data.integer.is_signed ? (uint32_t)2652528194 : (uint32_t)163929201) +
                    (((uint32_t)x.data.integer.bit_count) ^ (uint32_t)2998081557);
        case ZigTypeIdVector:
            return hash_ptr(x.data.vector.elem_type) * (x.data.vector.len * 526582681);
    }
    zig_unreachable();
}

bool type_id_eql(TypeId a, TypeId b) {
    if (a.id != b.id)
        return false;
    switch (a.id) {
        case ZigTypeIdInvalid:
        case ZigTypeIdMetaType:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdUnreachable:
        case ZigTypeIdFloat:
        case ZigTypeIdStruct:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdOptional:
        case ZigTypeIdErrorSet:
        case ZigTypeIdEnum:
        case ZigTypeIdUnion:
        case ZigTypeIdFn:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
        case ZigTypeIdOpaque:
        case ZigTypeIdFnFrame:
        case ZigTypeIdAnyFrame:
            zig_unreachable();
        case ZigTypeIdErrorUnion:
            return a.data.error_union.err_set_type == b.data.error_union.err_set_type &&
                a.data.error_union.payload_type == b.data.error_union.payload_type;

        case ZigTypeIdPointer:
            return a.data.pointer.child_type == b.data.pointer.child_type &&
                a.data.pointer.ptr_len == b.data.pointer.ptr_len &&
                a.data.pointer.is_const == b.data.pointer.is_const &&
                a.data.pointer.is_volatile == b.data.pointer.is_volatile &&
                a.data.pointer.allow_zero == b.data.pointer.allow_zero &&
                a.data.pointer.alignment == b.data.pointer.alignment &&
                a.data.pointer.bit_offset_in_host == b.data.pointer.bit_offset_in_host &&
                a.data.pointer.host_int_bytes == b.data.pointer.host_int_bytes;
        case ZigTypeIdArray:
            return a.data.array.child_type == b.data.array.child_type &&
                a.data.array.size == b.data.array.size;
        case ZigTypeIdInt:
            return a.data.integer.is_signed == b.data.integer.is_signed &&
                a.data.integer.bit_count == b.data.integer.bit_count;
        case ZigTypeIdVector:
            return a.data.vector.elem_type == b.data.vector.elem_type &&
                a.data.vector.len == b.data.vector.len;
    }
    zig_unreachable();
}

uint32_t zig_llvm_fn_key_hash(ZigLLVMFnKey x) {
    switch (x.id) {
        case ZigLLVMFnIdCtz:
            return (uint32_t)(x.data.ctz.bit_count) * (uint32_t)810453934;
        case ZigLLVMFnIdClz:
            return (uint32_t)(x.data.clz.bit_count) * (uint32_t)2428952817;
        case ZigLLVMFnIdPopCount:
            return (uint32_t)(x.data.clz.bit_count) * (uint32_t)101195049;
        case ZigLLVMFnIdFloatOp:
            return (uint32_t)(x.data.floating.bit_count) * ((uint32_t)x.id + 1025) +
                   (uint32_t)(x.data.floating.vector_len) * (((uint32_t)x.id << 5) + 1025) +
                   (uint32_t)(x.data.floating.op) * (uint32_t)43789879;
        case ZigLLVMFnIdFMA:
            return (uint32_t)(x.data.floating.bit_count) * ((uint32_t)x.id + 1025) +
                   (uint32_t)(x.data.floating.vector_len) * (((uint32_t)x.id << 5) + 1025);
        case ZigLLVMFnIdBswap:
            return (uint32_t)(x.data.bswap.bit_count) * ((uint32_t)3661994335) +
                   (uint32_t)(x.data.bswap.vector_len) * (((uint32_t)x.id << 5) + 1025);
        case ZigLLVMFnIdBitReverse:
            return (uint32_t)(x.data.bit_reverse.bit_count) * (uint32_t)2621398431;
        case ZigLLVMFnIdOverflowArithmetic:
            return ((uint32_t)(x.data.overflow_arithmetic.bit_count) * 87135777) +
                ((uint32_t)(x.data.overflow_arithmetic.add_sub_mul) * 31640542) +
                ((uint32_t)(x.data.overflow_arithmetic.is_signed) ? 1062315172 : 314955820) +
                x.data.overflow_arithmetic.vector_len * 1435156945;
    }
    zig_unreachable();
}

bool zig_llvm_fn_key_eql(ZigLLVMFnKey a, ZigLLVMFnKey b) {
    if (a.id != b.id)
        return false;
    switch (a.id) {
        case ZigLLVMFnIdCtz:
            return a.data.ctz.bit_count == b.data.ctz.bit_count;
        case ZigLLVMFnIdClz:
            return a.data.clz.bit_count == b.data.clz.bit_count;
        case ZigLLVMFnIdPopCount:
            return a.data.pop_count.bit_count == b.data.pop_count.bit_count;
        case ZigLLVMFnIdBswap:
            return a.data.bswap.bit_count == b.data.bswap.bit_count &&
                   a.data.bswap.vector_len == b.data.bswap.vector_len;
        case ZigLLVMFnIdBitReverse:
            return a.data.bit_reverse.bit_count == b.data.bit_reverse.bit_count;
        case ZigLLVMFnIdFloatOp:
            return a.data.floating.bit_count == b.data.floating.bit_count &&
                   a.data.floating.vector_len == b.data.floating.vector_len &&
                   a.data.floating.op == b.data.floating.op;
        case ZigLLVMFnIdFMA:
            return a.data.floating.bit_count == b.data.floating.bit_count &&
                   a.data.floating.vector_len == b.data.floating.vector_len;
        case ZigLLVMFnIdOverflowArithmetic:
            return (a.data.overflow_arithmetic.bit_count == b.data.overflow_arithmetic.bit_count) &&
                (a.data.overflow_arithmetic.add_sub_mul == b.data.overflow_arithmetic.add_sub_mul) &&
                (a.data.overflow_arithmetic.is_signed == b.data.overflow_arithmetic.is_signed) &&
                (a.data.overflow_arithmetic.vector_len == b.data.overflow_arithmetic.vector_len);
    }
    zig_unreachable();
}

static void init_const_undefined(CodeGen *g, ConstExprValue *const_val) {
    Error err;
    ZigType *wanted_type = const_val->type;
    if (wanted_type->id == ZigTypeIdArray) {
        const_val->special = ConstValSpecialStatic;
        const_val->data.x_array.special = ConstArraySpecialUndef;
    } else if (wanted_type->id == ZigTypeIdStruct) {
        if ((err = type_resolve(g, wanted_type, ResolveStatusZeroBitsKnown))) {
            return;
        }

        const_val->special = ConstValSpecialStatic;
        size_t field_count = wanted_type->data.structure.src_field_count;
        const_val->data.x_struct.fields = create_const_vals(field_count);
        for (size_t i = 0; i < field_count; i += 1) {
            ConstExprValue *field_val = &const_val->data.x_struct.fields[i];
            field_val->type = wanted_type->data.structure.fields[i].type_entry;
            assert(field_val->type);
            init_const_undefined(g, field_val);
            field_val->parent.id = ConstParentIdStruct;
            field_val->parent.data.p_struct.struct_val = const_val;
            field_val->parent.data.p_struct.field_index = i;
        }
    } else {
        const_val->special = ConstValSpecialUndef;
    }
}

void expand_undef_struct(CodeGen *g, ConstExprValue *const_val) {
    if (const_val->special == ConstValSpecialUndef) {
        init_const_undefined(g, const_val);
    }
}

// Canonicalize the array value as ConstArraySpecialNone
void expand_undef_array(CodeGen *g, ConstExprValue *const_val) {
    size_t elem_count;
    ZigType *elem_type;
    if (const_val->type->id == ZigTypeIdArray) {
        elem_count = const_val->type->data.array.len;
        elem_type = const_val->type->data.array.child_type;
    } else if (const_val->type->id == ZigTypeIdVector) {
        elem_count = const_val->type->data.vector.len;
        elem_type = const_val->type->data.vector.elem_type;
    } else {
        zig_unreachable();
    }
    if (const_val->special == ConstValSpecialUndef) {
        const_val->special = ConstValSpecialStatic;
        const_val->data.x_array.special = ConstArraySpecialUndef;
    }
    switch (const_val->data.x_array.special) {
        case ConstArraySpecialNone:
            return;
        case ConstArraySpecialUndef: {
            const_val->data.x_array.special = ConstArraySpecialNone;
            const_val->data.x_array.data.s_none.elements = create_const_vals(elem_count);
            for (size_t i = 0; i < elem_count; i += 1) {
                ConstExprValue *element_val = &const_val->data.x_array.data.s_none.elements[i];
                element_val->type = elem_type;
                init_const_undefined(g, element_val);
                element_val->parent.id = ConstParentIdArray;
                element_val->parent.data.p_array.array_val = const_val;
                element_val->parent.data.p_array.elem_index = i;
            }
            return;
        }
        case ConstArraySpecialBuf: {
            Buf *buf = const_val->data.x_array.data.s_buf;
            // If we're doing this it means that we are potentially modifying the data,
            // so we can't have it be in the string literals table
            g->string_literals_table.maybe_remove(buf);

            const_val->data.x_array.special = ConstArraySpecialNone;
            assert(elem_count == buf_len(buf));
            const_val->data.x_array.data.s_none.elements = create_const_vals(elem_count);
            for (size_t i = 0; i < elem_count; i += 1) {
                ConstExprValue *this_char = &const_val->data.x_array.data.s_none.elements[i];
                this_char->special = ConstValSpecialStatic;
                this_char->type = g->builtin_types.entry_u8;
                bigint_init_unsigned(&this_char->data.x_bigint, (uint8_t)buf_ptr(buf)[i]);
                this_char->parent.id = ConstParentIdArray;
                this_char->parent.data.p_array.array_val = const_val;
                this_char->parent.data.p_array.elem_index = i;
            }
            return;
        }
    }
    zig_unreachable();
}

static const ZigTypeId all_type_ids[] = {
    ZigTypeIdMetaType,
    ZigTypeIdVoid,
    ZigTypeIdBool,
    ZigTypeIdUnreachable,
    ZigTypeIdInt,
    ZigTypeIdFloat,
    ZigTypeIdPointer,
    ZigTypeIdArray,
    ZigTypeIdStruct,
    ZigTypeIdComptimeFloat,
    ZigTypeIdComptimeInt,
    ZigTypeIdUndefined,
    ZigTypeIdNull,
    ZigTypeIdOptional,
    ZigTypeIdErrorUnion,
    ZigTypeIdErrorSet,
    ZigTypeIdEnum,
    ZigTypeIdUnion,
    ZigTypeIdFn,
    ZigTypeIdBoundFn,
    ZigTypeIdArgTuple,
    ZigTypeIdOpaque,
    ZigTypeIdFnFrame,
    ZigTypeIdAnyFrame,
    ZigTypeIdVector,
    ZigTypeIdEnumLiteral,
};

ZigTypeId type_id_at_index(size_t index) {
    assert(index < array_length(all_type_ids));
    return all_type_ids[index];
}

size_t type_id_len() {
    return array_length(all_type_ids);
}

size_t type_id_index(ZigType *entry) {
    switch (entry->id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdMetaType:
            return 0;
        case ZigTypeIdVoid:
            return 1;
        case ZigTypeIdBool:
            return 2;
        case ZigTypeIdUnreachable:
            return 3;
        case ZigTypeIdInt:
            return 4;
        case ZigTypeIdFloat:
            return 5;
        case ZigTypeIdPointer:
            return 6;
        case ZigTypeIdArray:
            return 7;
        case ZigTypeIdStruct:
            if (entry->data.structure.is_slice)
                return 6;
            return 8;
        case ZigTypeIdComptimeFloat:
            return 9;
        case ZigTypeIdComptimeInt:
            return 10;
        case ZigTypeIdUndefined:
            return 11;
        case ZigTypeIdNull:
            return 12;
        case ZigTypeIdOptional:
            return 13;
        case ZigTypeIdErrorUnion:
            return 14;
        case ZigTypeIdErrorSet:
            return 15;
        case ZigTypeIdEnum:
            return 16;
        case ZigTypeIdUnion:
            return 17;
        case ZigTypeIdFn:
            return 18;
        case ZigTypeIdBoundFn:
            return 19;
        case ZigTypeIdArgTuple:
            return 20;
        case ZigTypeIdOpaque:
            return 21;
        case ZigTypeIdFnFrame:
            return 22;
        case ZigTypeIdAnyFrame:
            return 23;
        case ZigTypeIdVector:
            return 24;
        case ZigTypeIdEnumLiteral:
            return 25;
    }
    zig_unreachable();
}

const char *type_id_name(ZigTypeId id) {
    switch (id) {
        case ZigTypeIdInvalid:
            zig_unreachable();
        case ZigTypeIdMetaType:
            return "Type";
        case ZigTypeIdVoid:
            return "Void";
        case ZigTypeIdBool:
            return "Bool";
        case ZigTypeIdUnreachable:
            return "NoReturn";
        case ZigTypeIdInt:
            return "Int";
        case ZigTypeIdFloat:
            return "Float";
        case ZigTypeIdPointer:
            return "Pointer";
        case ZigTypeIdArray:
            return "Array";
        case ZigTypeIdStruct:
            return "Struct";
        case ZigTypeIdComptimeFloat:
            return "ComptimeFloat";
        case ZigTypeIdComptimeInt:
            return "ComptimeInt";
        case ZigTypeIdEnumLiteral:
            return "EnumLiteral";
        case ZigTypeIdUndefined:
            return "Undefined";
        case ZigTypeIdNull:
            return "Null";
        case ZigTypeIdOptional:
            return "Optional";
        case ZigTypeIdErrorUnion:
            return "ErrorUnion";
        case ZigTypeIdErrorSet:
            return "ErrorSet";
        case ZigTypeIdEnum:
            return "Enum";
        case ZigTypeIdUnion:
            return "Union";
        case ZigTypeIdFn:
            return "Fn";
        case ZigTypeIdBoundFn:
            return "BoundFn";
        case ZigTypeIdArgTuple:
            return "ArgTuple";
        case ZigTypeIdOpaque:
            return "Opaque";
        case ZigTypeIdVector:
            return "Vector";
        case ZigTypeIdFnFrame:
            return "Frame";
        case ZigTypeIdAnyFrame:
            return "AnyFrame";
    }
    zig_unreachable();
}

LinkLib *create_link_lib(Buf *name) {
    LinkLib *link_lib = allocate<LinkLib>(1);
    link_lib->name = name;
    return link_lib;
}

LinkLib *add_link_lib(CodeGen *g, Buf *name) {
    bool is_libc = buf_eql_str(name, "c");

    if (is_libc && g->libc_link_lib != nullptr)
        return g->libc_link_lib;

    for (size_t i = 0; i < g->link_libs_list.length; i += 1) {
        LinkLib *existing_lib = g->link_libs_list.at(i);
        if (buf_eql_buf(existing_lib->name, name)) {
            return existing_lib;
        }
    }

    LinkLib *link_lib = create_link_lib(name);
    g->link_libs_list.append(link_lib);

    if (is_libc)
        g->libc_link_lib = link_lib;

    return link_lib;
}

ZigType *get_align_amt_type(CodeGen *g) {
    if (g->align_amt_type == nullptr) {
        // according to LLVM the maximum alignment is 1 << 29.
        g->align_amt_type = get_int_type(g, false, 29);
    }
    return g->align_amt_type;
}

uint32_t type_ptr_hash(const ZigType *ptr) {
    return hash_ptr((void*)ptr);
}

bool type_ptr_eql(const ZigType *a, const ZigType *b) {
    return a == b;
}

uint32_t pkg_ptr_hash(const ZigPackage *ptr) {
    return hash_ptr((void*)ptr);
}

bool pkg_ptr_eql(const ZigPackage *a, const ZigPackage *b) {
    return a == b;
}

uint32_t tld_ptr_hash(const Tld *ptr) {
    return hash_ptr((void*)ptr);
}

bool tld_ptr_eql(const Tld *a, const Tld *b) {
    return a == b;
}

uint32_t node_ptr_hash(const AstNode *ptr) {
    return hash_ptr((void*)ptr);
}

bool node_ptr_eql(const AstNode *a, const AstNode *b) {
    return a == b;
}

uint32_t fn_ptr_hash(const ZigFn *ptr) {
    return hash_ptr((void*)ptr);
}

bool fn_ptr_eql(const ZigFn *a, const ZigFn *b) {
    return a == b;
}

uint32_t err_ptr_hash(const ErrorTableEntry *ptr) {
    return hash_ptr((void*)ptr);
}

bool err_ptr_eql(const ErrorTableEntry *a, const ErrorTableEntry *b) {
    return a == b;
}

ConstExprValue *get_builtin_value(CodeGen *codegen, const char *name) {
    Tld *tld = get_container_scope(codegen->compile_var_import)->decl_table.get(buf_create_from_str(name));
    resolve_top_level_decl(codegen, tld, nullptr, false);
    assert(tld->id == TldIdVar);
    TldVar *tld_var = (TldVar *)tld;
    ConstExprValue *var_value = tld_var->var->const_value;
    assert(var_value != nullptr);
    return var_value;
}

bool type_is_global_error_set(ZigType *err_set_type) {
    assert(err_set_type->id == ZigTypeIdErrorSet);
    assert(!err_set_type->data.error_set.incomplete);
    return err_set_type->data.error_set.err_count == UINT32_MAX;
}

bool type_can_fail(ZigType *type_entry) {
    return type_entry->id == ZigTypeIdErrorUnion || type_entry->id == ZigTypeIdErrorSet;
}

bool fn_type_can_fail(FnTypeId *fn_type_id) {
    return type_can_fail(fn_type_id->return_type);
}

// ErrorNone - result pointer has the type
// ErrorOverflow - an integer primitive type has too large a bit width
// ErrorPrimitiveTypeNotFound - result pointer unchanged
Error get_primitive_type(CodeGen *g, Buf *name, ZigType **result) {
    if (buf_len(name) >= 2) {
        uint8_t first_c = buf_ptr(name)[0];
        if (first_c == 'i' || first_c == 'u') {
            for (size_t i = 1; i < buf_len(name); i += 1) {
                uint8_t c = buf_ptr(name)[i];
                if (c < '0' || c > '9') {
                    goto not_integer;
                }
            }
            bool is_signed = (first_c == 'i');
            unsigned long int bit_count = strtoul(buf_ptr(name) + 1, nullptr, 10);
            // strtoul returns ULONG_MAX on errors, so this comparison catches that as well.
            if (bit_count >= 65536) return ErrorOverflow;
            *result = get_int_type(g, is_signed, bit_count);
            return ErrorNone;
        }
    }

not_integer:

    auto primitive_table_entry = g->primitive_type_table.maybe_get(name);
    if (primitive_table_entry == nullptr)
        return ErrorPrimitiveTypeNotFound;

    *result = primitive_table_entry->value;
    return ErrorNone;
}

Error file_fetch(CodeGen *g, Buf *resolved_path, Buf *contents) {
    if (g->enable_cache) {
        return cache_add_file_fetch(&g->cache_hash, resolved_path, contents);
    } else {
        return os_fetch_file_path(resolved_path, contents);
    }
}

static X64CABIClass type_windows_abi_x86_64_class(CodeGen *g, ZigType *ty, size_t ty_size) {
    // https://docs.microsoft.com/en-gb/cpp/build/x64-calling-convention?view=vs-2017
    switch (ty->id) {
        case ZigTypeIdEnum:
        case ZigTypeIdInt:
        case ZigTypeIdBool:
            return X64CABIClass_INTEGER;
        case ZigTypeIdFloat:
        case ZigTypeIdVector:
            return X64CABIClass_SSE;
        case ZigTypeIdStruct:
        case ZigTypeIdUnion: {
            if (ty_size <= 8)
                return X64CABIClass_INTEGER;
            return X64CABIClass_MEMORY;
        }
        default:
            return X64CABIClass_Unknown;
    }
}

static X64CABIClass type_system_V_abi_x86_64_class(CodeGen *g, ZigType *ty, size_t ty_size) {
    switch (ty->id) {
        case ZigTypeIdEnum:
        case ZigTypeIdInt:
        case ZigTypeIdBool:
            return X64CABIClass_INTEGER;
        case ZigTypeIdFloat:
        case ZigTypeIdVector:
            return X64CABIClass_SSE;
        case ZigTypeIdStruct: {
            // "If the size of an object is larger than four eightbytes, or it contains unaligned
            // fields, it has class MEMORY"
            if (ty_size > 32)
                return X64CABIClass_MEMORY;
            if (ty->data.structure.layout != ContainerLayoutExtern) {
                // TODO determine whether packed structs have any unaligned fields
                return X64CABIClass_Unknown;
            }
            // "If the size of the aggregate exceeds two eightbytes and the first eight-
            // byte isn’t SSE or any other eightbyte isn’t SSEUP, the whole argument
            // is passed in memory."
            if (ty_size > 16) {
                // Zig doesn't support vectors and large fp registers yet, so this will always
                // be memory.
                return X64CABIClass_MEMORY;
            }
            X64CABIClass working_class = X64CABIClass_Unknown;
            for (uint32_t i = 0; i < ty->data.structure.src_field_count; i += 1) {
                X64CABIClass field_class = type_c_abi_x86_64_class(g, ty->data.structure.fields->type_entry);
                if (field_class == X64CABIClass_Unknown)
                    return X64CABIClass_Unknown;
                if (i == 0 || field_class == X64CABIClass_MEMORY || working_class == X64CABIClass_SSE) {
                    working_class = field_class;
                }
            }
            return working_class;
        }
        case ZigTypeIdUnion: {
            // "If the size of an object is larger than four eightbytes, or it contains unaligned
            // fields, it has class MEMORY"
            if (ty_size > 32)
                return X64CABIClass_MEMORY;
            if (ty->data.unionation.layout != ContainerLayoutExtern)
                return X64CABIClass_MEMORY;
            // "If the size of the aggregate exceeds two eightbytes and the first eight-
            // byte isn’t SSE or any other eightbyte isn’t SSEUP, the whole argument
            // is passed in memory."
            if (ty_size > 16) {
                // Zig doesn't support vectors and large fp registers yet, so this will always
                // be memory.
                return X64CABIClass_MEMORY;
            }
            X64CABIClass working_class = X64CABIClass_Unknown;
            for (uint32_t i = 0; i < ty->data.unionation.src_field_count; i += 1) {
                X64CABIClass field_class = type_c_abi_x86_64_class(g, ty->data.unionation.fields->type_entry);
                if (field_class == X64CABIClass_Unknown)
                    return X64CABIClass_Unknown;
                if (i == 0 || field_class == X64CABIClass_MEMORY || working_class == X64CABIClass_SSE) {
                    working_class = field_class;
                }
            }
            return working_class;
        }
        default:
            return X64CABIClass_Unknown;
    }
}

X64CABIClass type_c_abi_x86_64_class(CodeGen *g, ZigType *ty) {
    const size_t ty_size = type_size(g, ty);
    if (get_codegen_ptr_type(ty) != nullptr)
        return X64CABIClass_INTEGER;

    if (g->zig_target->os == OsWindows || g->zig_target->os == OsUefi) {
        return type_windows_abi_x86_64_class(g, ty, ty_size);
    } else if (g->zig_target->arch == ZigLLVM_aarch64 ||
            g->zig_target->arch == ZigLLVM_aarch64_be)
    {
        X64CABIClass result = type_system_V_abi_x86_64_class(g, ty, ty_size);
        return (result == X64CABIClass_MEMORY) ? X64CABIClass_MEMORY_nobyval : result;
    } else {
        return type_system_V_abi_x86_64_class(g, ty, ty_size);
    }
}

// NOTE this does not depend on x86_64
bool type_is_c_abi_int(CodeGen *g, ZigType *ty) {
    return (ty->id == ZigTypeIdInt ||
        ty->id == ZigTypeIdFloat ||
        ty->id == ZigTypeIdBool ||
        ty->id == ZigTypeIdEnum ||
        ty->id == ZigTypeIdVoid ||
        ty->id == ZigTypeIdUnreachable ||
        get_codegen_ptr_type(ty) != nullptr);
}

uint32_t get_host_int_bytes(CodeGen *g, ZigType *struct_type, TypeStructField *field) {
    assert(struct_type->id == ZigTypeIdStruct);
    if (struct_type->data.structure.layout != ContainerLayoutAuto) {
        assert(type_is_resolved(struct_type, ResolveStatusSizeKnown));
    }
    if (struct_type->data.structure.host_int_bytes == nullptr)
        return 0;
    return struct_type->data.structure.host_int_bytes[field->gen_index];
}

Error ensure_const_val_repr(IrAnalyze *ira, CodeGen *codegen, AstNode *source_node,
        ConstExprValue *const_val, ZigType *wanted_type)
{
    ConstExprValue ptr_val = {};
    ptr_val.special = ConstValSpecialStatic;
    ptr_val.type = get_pointer_to_type(codegen, wanted_type, true);
    ptr_val.data.x_ptr.mut = ConstPtrMutComptimeConst;
    ptr_val.data.x_ptr.special = ConstPtrSpecialRef;
    ptr_val.data.x_ptr.data.ref.pointee = const_val;
    if (const_ptr_pointee(ira, codegen, &ptr_val, source_node) == nullptr)
        return ErrorSemanticAnalyzeFail;

    return ErrorNone;
}

const char *container_string(ContainerKind kind) {
    switch (kind) {
        case ContainerKindEnum: return "enum";
        case ContainerKindStruct: return "struct";
        case ContainerKindUnion: return "union";
    }
    zig_unreachable();
}

bool ptr_allows_addr_zero(ZigType *ptr_type) {
    if (ptr_type->id == ZigTypeIdPointer) {
        return ptr_type->data.pointer.allow_zero;
    } else if (ptr_type->id == ZigTypeIdOptional) {
        return true;
    }
    return false;
}

Buf *type_bare_name(ZigType *type_entry) {
    if (is_slice(type_entry)) {
        return &type_entry->name;
    } else if (is_container(type_entry)) {
        return get_container_scope(type_entry)->bare_name;
    } else if (type_entry->id == ZigTypeIdOpaque) {
        return type_entry->data.opaque.bare_name;
    } else {
        return &type_entry->name;
    }
}

// TODO this will have to be more clever, probably using the full name
// and replacing '.' with '_' or something like that
Buf *type_h_name(ZigType *t) {
    return type_bare_name(t);
}

static void resolve_llvm_types_slice(CodeGen *g, ZigType *type, ResolveStatus wanted_resolve_status) {
    if (type->data.structure.resolve_status >= wanted_resolve_status) return;

    ZigType *ptr_type = type->data.structure.fields[slice_ptr_index].type_entry;
    ZigType *child_type = ptr_type->data.pointer.child_type;
    ZigType *usize_type = g->builtin_types.entry_usize;

    bool done = false;
    if (ptr_type->data.pointer.is_const || ptr_type->data.pointer.is_volatile ||
        ptr_type->data.pointer.explicit_alignment != 0 || ptr_type->data.pointer.allow_zero)
    {
        ZigType *peer_ptr_type = get_pointer_to_type_extra(g, child_type, false, false,
                PtrLenUnknown, 0, 0, 0, false);
        ZigType *peer_slice_type = get_slice_type(g, peer_ptr_type);

        assertNoError(type_resolve(g, peer_slice_type, wanted_resolve_status));
        type->llvm_type = peer_slice_type->llvm_type;
        type->llvm_di_type = peer_slice_type->llvm_di_type;
        type->data.structure.resolve_status = peer_slice_type->data.structure.resolve_status;
        done = true;
    }

    // If the child type is []const T then we need to make sure the type ref
    // and debug info is the same as if the child type were []T.
    if (is_slice(child_type)) {
        ZigType *child_ptr_type = child_type->data.structure.fields[slice_ptr_index].type_entry;
        assert(child_ptr_type->id == ZigTypeIdPointer);
        if (child_ptr_type->data.pointer.is_const || child_ptr_type->data.pointer.is_volatile ||
            child_ptr_type->data.pointer.explicit_alignment != 0 || child_ptr_type->data.pointer.allow_zero)
        {
            ZigType *grand_child_type = child_ptr_type->data.pointer.child_type;
            ZigType *bland_child_ptr_type = get_pointer_to_type_extra(g, grand_child_type, false, false,
                    PtrLenUnknown, 0, 0, 0, false);
            ZigType *bland_child_slice = get_slice_type(g, bland_child_ptr_type);
            ZigType *peer_ptr_type = get_pointer_to_type_extra(g, bland_child_slice, false, false,
                    PtrLenUnknown, 0, 0, 0, false);
            ZigType *peer_slice_type = get_slice_type(g, peer_ptr_type);

            assertNoError(type_resolve(g, peer_slice_type, wanted_resolve_status));
            type->llvm_type = peer_slice_type->llvm_type;
            type->llvm_di_type = peer_slice_type->llvm_di_type;
            type->data.structure.resolve_status = peer_slice_type->data.structure.resolve_status;
            done = true;
        }
    }

    if (done) return;

    LLVMTypeRef usize_llvm_type = get_llvm_type(g, usize_type);
    ZigLLVMDIType *usize_llvm_di_type = get_llvm_di_type(g, usize_type);
    ZigLLVMDIScope *compile_unit_scope = ZigLLVMCompileUnitToScope(g->compile_unit);
    ZigLLVMDIFile *di_file = nullptr;
    unsigned line = 0;

    if (type->data.structure.resolve_status < ResolveStatusLLVMFwdDecl) {
        type->llvm_type = LLVMStructCreateNamed(LLVMGetGlobalContext(), buf_ptr(&type->name));

        type->llvm_di_type = ZigLLVMCreateReplaceableCompositeType(g->dbuilder,
            ZigLLVMTag_DW_structure_type(), buf_ptr(&type->name),
            compile_unit_scope, di_file, line);

        type->data.structure.resolve_status = ResolveStatusLLVMFwdDecl;
        if (ResolveStatusLLVMFwdDecl >= wanted_resolve_status) return;
    }

    if (!type_has_bits(child_type)) {
        LLVMTypeRef element_types[] = {
            usize_llvm_type,
        };
        LLVMStructSetBody(type->llvm_type, element_types, 1, false);

        uint64_t len_debug_size_in_bits = usize_type->size_in_bits;
        uint64_t len_debug_align_in_bits = 8*usize_type->abi_align;
        uint64_t len_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, type->llvm_type, 0);

        uint64_t debug_size_in_bits = type->size_in_bits;
        uint64_t debug_align_in_bits = 8*type->abi_align;

        ZigLLVMDIType *di_element_types[] = {
            ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(type->llvm_di_type),
                    "len", di_file, line,
                    len_debug_size_in_bits,
                    len_debug_align_in_bits,
                    len_offset_in_bits,
                    ZigLLVM_DIFlags_Zero,
                    usize_llvm_di_type),
        };
        ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
                compile_unit_scope,
                buf_ptr(&type->name),
                di_file, line, debug_size_in_bits, debug_align_in_bits,
                ZigLLVM_DIFlags_Zero,
                nullptr, di_element_types, 1, 0, nullptr, "");

        ZigLLVMReplaceTemporary(g->dbuilder, type->llvm_di_type, replacement_di_type);
        type->llvm_di_type = replacement_di_type;
        type->data.structure.resolve_status = ResolveStatusLLVMFull;
        return;
    }

    LLVMTypeRef element_types[2];
    element_types[slice_ptr_index] = get_llvm_type(g, ptr_type);
    element_types[slice_len_index] = get_llvm_type(g, g->builtin_types.entry_usize);
    if (type->data.structure.resolve_status >= wanted_resolve_status) return;
    LLVMStructSetBody(type->llvm_type, element_types, 2, false);

    uint64_t ptr_debug_size_in_bits = ptr_type->size_in_bits;
    uint64_t ptr_debug_align_in_bits = 8*ptr_type->abi_align;
    uint64_t ptr_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, type->llvm_type, 0);

    uint64_t len_debug_size_in_bits = usize_type->size_in_bits;
    uint64_t len_debug_align_in_bits = 8*usize_type->abi_align;
    uint64_t len_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, type->llvm_type, 1);

    uint64_t debug_size_in_bits = type->size_in_bits;
    uint64_t debug_align_in_bits = 8*type->abi_align;

    ZigLLVMDIType *di_element_types[] = {
        ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(type->llvm_di_type),
                "ptr", di_file, line,
                ptr_debug_size_in_bits,
                ptr_debug_align_in_bits,
                ptr_offset_in_bits,
                ZigLLVM_DIFlags_Zero, get_llvm_di_type(g, ptr_type)),
        ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(type->llvm_di_type),
                "len", di_file, line,
                len_debug_size_in_bits,
                len_debug_align_in_bits,
                len_offset_in_bits,
                ZigLLVM_DIFlags_Zero, usize_llvm_di_type),
    };
    ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
            compile_unit_scope,
            buf_ptr(&type->name),
            di_file, line, debug_size_in_bits, debug_align_in_bits,
            ZigLLVM_DIFlags_Zero,
            nullptr, di_element_types, 2, 0, nullptr, "");

    ZigLLVMReplaceTemporary(g->dbuilder, type->llvm_di_type, replacement_di_type);
    type->llvm_di_type = replacement_di_type;
    type->data.structure.resolve_status = ResolveStatusLLVMFull;
}

static LLVMTypeRef get_llvm_type_of_n_bytes(unsigned byte_size) {
    return byte_size == 1 ?
        LLVMInt8Type() : LLVMArrayType(LLVMInt8Type(), byte_size);
}

static void resolve_llvm_types_struct(CodeGen *g, ZigType *struct_type, ResolveStatus wanted_resolve_status,
        ZigType *async_frame_type)
{
    assert(struct_type->id == ZigTypeIdStruct);
    assert(struct_type->data.structure.resolve_status != ResolveStatusInvalid);
    assert(struct_type->data.structure.resolve_status >= ResolveStatusSizeKnown);
    assert(struct_type->data.structure.fields || struct_type->data.structure.src_field_count == 0);
    if (struct_type->data.structure.resolve_status >= wanted_resolve_status) return;

    AstNode *decl_node = struct_type->data.structure.decl_node;
    ZigLLVMDIFile *di_file;
    ZigLLVMDIScope *di_scope;
    unsigned line;
    if (decl_node != nullptr) {
        assert(decl_node->type == NodeTypeContainerDecl);
        Scope *scope = &struct_type->data.structure.decls_scope->base;
        ZigType *import = get_scope_import(scope);
        di_file = import->data.structure.root_struct->di_file;
        di_scope = ZigLLVMFileToScope(di_file);
        line = decl_node->line + 1;
    } else {
        di_file = nullptr;
        di_scope = ZigLLVMCompileUnitToScope(g->compile_unit);
        line = 0;
    }

    if (struct_type->data.structure.resolve_status < ResolveStatusLLVMFwdDecl) {
        struct_type->llvm_type = type_has_bits(struct_type) ?
            LLVMStructCreateNamed(LLVMGetGlobalContext(), buf_ptr(&struct_type->name)) : LLVMVoidType();
        unsigned dwarf_kind = ZigLLVMTag_DW_structure_type();
        struct_type->llvm_di_type = ZigLLVMCreateReplaceableCompositeType(g->dbuilder,
            dwarf_kind, buf_ptr(&struct_type->name),
            di_scope, di_file, line);

        struct_type->data.structure.resolve_status = ResolveStatusLLVMFwdDecl;
        if (ResolveStatusLLVMFwdDecl >= wanted_resolve_status) {
            struct_type->data.structure.llvm_full_type_queue_index = g->type_resolve_stack.length;
            g->type_resolve_stack.append(struct_type);
            return;
        } else {
            struct_type->data.structure.llvm_full_type_queue_index = SIZE_MAX;
        }
    }

    size_t field_count = struct_type->data.structure.src_field_count;
    // Every field could potentially have a generated padding field after it.
    LLVMTypeRef *element_types = allocate<LLVMTypeRef>(field_count * 2);

    bool packed = (struct_type->data.structure.layout == ContainerLayoutPacked);
    size_t packed_bits_offset = 0;
    size_t first_packed_bits_offset_misalign = SIZE_MAX;
    size_t debug_field_count = 0;

    // trigger all the recursive get_llvm_type calls
    for (size_t i = 0; i < field_count; i += 1) {
        TypeStructField *field = &struct_type->data.structure.fields[i];
        ZigType *field_type = field->type_entry;
        if (!type_has_bits(field_type))
            continue;
        (void)get_llvm_type(g, field_type);
        if (struct_type->data.structure.resolve_status >= wanted_resolve_status) return;
    }

    size_t gen_field_index = 0;

    // Calculate what LLVM thinks the ABI align of the struct will be. We do this to avoid
    // inserting padding bytes where LLVM would do it automatically.
    size_t llvm_struct_abi_align = 0;
    for (size_t i = 0; i < field_count; i += 1) {
        ZigType *field_type = struct_type->data.structure.fields[i].type_entry;
        if (!type_has_bits(field_type))
            continue;
        LLVMTypeRef field_llvm_type = get_llvm_type(g, field_type);
        size_t llvm_field_abi_align = LLVMABIAlignmentOfType(g->target_data_ref, field_llvm_type);
        llvm_struct_abi_align = max(llvm_struct_abi_align, llvm_field_abi_align);
    }

    for (size_t i = 0; i < field_count; i += 1) {
        TypeStructField *field = &struct_type->data.structure.fields[i];
        ZigType *field_type = field->type_entry;

        if (!type_has_bits(field_type)) {
            field->gen_index = SIZE_MAX;
            continue;
        }

        if (packed) {
            size_t field_size_in_bits = type_size_bits(g, field_type);
            size_t next_packed_bits_offset = packed_bits_offset + field_size_in_bits;

            if (first_packed_bits_offset_misalign != SIZE_MAX) {
                // this field is not byte-aligned; it is part of the previous field with a bit offset

                size_t full_bit_count = next_packed_bits_offset - first_packed_bits_offset_misalign;
                size_t full_abi_size = get_abi_size_bytes(full_bit_count, g->pointer_size_bytes);
                if (full_abi_size * 8 == full_bit_count) {
                    // next field recovers ABI alignment
                    element_types[gen_field_index] = get_llvm_type_of_n_bytes(full_abi_size);
                    gen_field_index += 1;
                    first_packed_bits_offset_misalign = SIZE_MAX;
                }
            } else if (get_abi_size_bytes(field_type->size_in_bits, g->pointer_size_bytes) * 8 != field_size_in_bits) {
                first_packed_bits_offset_misalign = packed_bits_offset;
            } else {
                // This is a byte-aligned field (both start and end) in a packed struct.
                element_types[gen_field_index] = get_llvm_type(g, field_type);
                assert(get_abi_size_bytes(field_type->size_in_bits, g->pointer_size_bytes) ==
                       LLVMStoreSizeOfType(g->target_data_ref, element_types[gen_field_index]));
                gen_field_index += 1;
            }
            packed_bits_offset = next_packed_bits_offset;
        } else {
            LLVMTypeRef llvm_type;
            if (i == 0 && async_frame_type != nullptr) {
                assert(async_frame_type->id == ZigTypeIdFnFrame);
                assert(field_type->id == ZigTypeIdFn);
                resolve_llvm_types_fn(g, async_frame_type->data.frame.fn);
                llvm_type = LLVMPointerType(async_frame_type->data.frame.fn->raw_type_ref, 0);
            } else {
                llvm_type = get_llvm_type(g, field_type);
            }
            element_types[gen_field_index] = llvm_type;
            field->gen_index = gen_field_index;
            gen_field_index += 1;

            // find the next non-zero-byte field for offset calculations
            size_t next_src_field_index = i + 1;
            for (; next_src_field_index < field_count; next_src_field_index += 1) {
                if (type_has_bits(struct_type->data.structure.fields[next_src_field_index].type_entry))
                    break;
            }
            size_t next_abi_align;
            if (next_src_field_index == field_count) {
                next_abi_align = struct_type->abi_align;
            } else {
                if (struct_type->data.structure.fields[next_src_field_index].align == 0) {
                    next_abi_align = struct_type->data.structure.fields[next_src_field_index].type_entry->abi_align;
                } else {
                    next_abi_align = struct_type->data.structure.fields[next_src_field_index].align;
                }
            }
            size_t llvm_next_abi_align = (next_src_field_index == field_count) ?
                llvm_struct_abi_align :
                LLVMABIAlignmentOfType(g->target_data_ref,
                        get_llvm_type(g, struct_type->data.structure.fields[next_src_field_index].type_entry));

            size_t next_offset = next_field_offset(field->offset, struct_type->abi_align,
                    field_type->abi_size, next_abi_align);
            size_t llvm_next_offset = next_field_offset(field->offset, llvm_struct_abi_align,
                    LLVMABISizeOfType(g->target_data_ref, llvm_type), llvm_next_abi_align);

            assert(next_offset >= llvm_next_offset);
            if (next_offset > llvm_next_offset) {
                size_t pad_bytes = next_offset - (field->offset + LLVMStoreSizeOfType(g->target_data_ref, llvm_type));
                if (pad_bytes != 0) {
                    LLVMTypeRef pad_llvm_type = LLVMArrayType(LLVMInt8Type(), pad_bytes);
                    element_types[gen_field_index] = pad_llvm_type;
                    gen_field_index += 1;
                }
            }
        }
        debug_field_count += 1;
    }
    if (!packed) {
        struct_type->data.structure.gen_field_count = gen_field_index;
    }

    if (first_packed_bits_offset_misalign != SIZE_MAX) {
        size_t full_bit_count = packed_bits_offset - first_packed_bits_offset_misalign;
        size_t full_abi_size = get_abi_size_bytes(full_bit_count, g->pointer_size_bytes);
        element_types[gen_field_index] = get_llvm_type_of_n_bytes(full_abi_size);
        gen_field_index += 1;
    }

    if (type_has_bits(struct_type)) {
        assert(struct_type->data.structure.gen_field_count == gen_field_index);
        LLVMStructSetBody(struct_type->llvm_type, element_types,
                (unsigned)struct_type->data.structure.gen_field_count, packed);
    }

    ZigLLVMDIType **di_element_types = allocate<ZigLLVMDIType*>(debug_field_count);
    size_t debug_field_index = 0;
    for (size_t i = 0; i < field_count; i += 1) {
        TypeStructField *field = &struct_type->data.structure.fields[i];
        size_t gen_field_index = field->gen_index;
        if (gen_field_index == SIZE_MAX) {
            continue;
        }

        ZigType *field_type = field->type_entry;

        // if the field is a function, actually the debug info should be a pointer.
        ZigLLVMDIType *field_di_type;
        if (field_type->id == ZigTypeIdFn) {
            ZigType *field_ptr_type = get_pointer_to_type(g, field_type, true);
            uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, get_llvm_type(g, field_ptr_type));
            uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, get_llvm_type(g, field_ptr_type));
            field_di_type = ZigLLVMCreateDebugPointerType(g->dbuilder, get_llvm_di_type(g, field_type),
                    debug_size_in_bits, debug_align_in_bits, buf_ptr(&field_ptr_type->name));
        } else {
            field_di_type = get_llvm_di_type(g, field_type);
        }

        uint64_t debug_size_in_bits;
        uint64_t debug_align_in_bits;
        uint64_t debug_offset_in_bits;
        if (packed) {
            debug_size_in_bits = field->type_entry->size_in_bits;
            debug_align_in_bits = 8 * field->type_entry->abi_align;
            debug_offset_in_bits = 8 * field->offset + field->bit_offset_in_host;
        } else {
            debug_size_in_bits = 8 * get_store_size_bytes(field_type->size_in_bits);
            debug_align_in_bits = 8 * field_type->abi_align;
            debug_offset_in_bits = 8 * field->offset;
        }
        unsigned line;
        if (decl_node != nullptr) {
            AstNode *field_node = decl_node->data.container_decl.fields.at(i);
            line = field_node->line + 1;
        } else {
            line = 0;
        }
        di_element_types[debug_field_index] = ZigLLVMCreateDebugMemberType(g->dbuilder,
                ZigLLVMTypeToScope(struct_type->llvm_di_type), buf_ptr(field->name),
                di_file, line,
                debug_size_in_bits,
                debug_align_in_bits,
                debug_offset_in_bits,
                ZigLLVM_DIFlags_Zero, field_di_type);
        assert(di_element_types[debug_field_index]);
        debug_field_index += 1;
    }

    uint64_t debug_size_in_bits = 8*get_store_size_bytes(struct_type->size_in_bits);
    uint64_t debug_align_in_bits = 8*struct_type->abi_align;
    ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
            di_scope,
            buf_ptr(&struct_type->name),
            di_file, line,
            debug_size_in_bits,
            debug_align_in_bits,
            ZigLLVM_DIFlags_Zero,
            nullptr, di_element_types, (int)debug_field_count, 0, nullptr, "");

    ZigLLVMReplaceTemporary(g->dbuilder, struct_type->llvm_di_type, replacement_di_type);
    struct_type->llvm_di_type = replacement_di_type;
    struct_type->data.structure.resolve_status = ResolveStatusLLVMFull;
    if (struct_type->data.structure.llvm_full_type_queue_index != SIZE_MAX) {
        ZigType *last = g->type_resolve_stack.last();
        assert(last->id == ZigTypeIdStruct);
        last->data.structure.llvm_full_type_queue_index = struct_type->data.structure.llvm_full_type_queue_index;
        g->type_resolve_stack.swap_remove(struct_type->data.structure.llvm_full_type_queue_index);
        struct_type->data.structure.llvm_full_type_queue_index = SIZE_MAX;
    }
}

// This is to be used instead of void for debug info types, to avoid tripping
// Assertion `!isa<DIType>(Scope) && "shouldn't make a namespace scope for a type"'
// when targeting CodeView (Windows).
static ZigLLVMDIType *make_empty_namespace_llvm_di_type(CodeGen *g, ZigType *import, const char *name,
        AstNode *decl_node)
{
    uint64_t debug_size_in_bits = 0;
    uint64_t debug_align_in_bits = 0;
    ZigLLVMDIType **di_element_types = nullptr;
    size_t debug_field_count = 0;
    return ZigLLVMCreateDebugStructType(g->dbuilder,
        ZigLLVMFileToScope(import->data.structure.root_struct->di_file),
        name,
        import->data.structure.root_struct->di_file, (unsigned)(decl_node->line + 1),
        debug_size_in_bits,
        debug_align_in_bits,
        ZigLLVM_DIFlags_Zero,
        nullptr, di_element_types, (int)debug_field_count, 0, nullptr, "");
}

static void resolve_llvm_types_enum(CodeGen *g, ZigType *enum_type, ResolveStatus wanted_resolve_status) {
    assert(enum_type->data.enumeration.resolve_status >= ResolveStatusSizeKnown);
    if (enum_type->data.enumeration.resolve_status >= wanted_resolve_status) return;

    Scope *scope = &enum_type->data.enumeration.decls_scope->base;
    ZigType *import = get_scope_import(scope);
    AstNode *decl_node = enum_type->data.enumeration.decl_node;

    if (!type_has_bits(enum_type)) {
        enum_type->llvm_type = g->builtin_types.entry_void->llvm_type;
        enum_type->llvm_di_type = make_empty_namespace_llvm_di_type(g, import, buf_ptr(&enum_type->name),
                decl_node);
        enum_type->data.enumeration.resolve_status = ResolveStatusLLVMFull;
        return;
    }

    uint32_t field_count = enum_type->data.enumeration.src_field_count;

    assert(enum_type->data.enumeration.fields);
    ZigLLVMDIEnumerator **di_enumerators = allocate<ZigLLVMDIEnumerator*>(field_count);

    for (uint32_t i = 0; i < field_count; i += 1) {
        TypeEnumField *enum_field = &enum_type->data.enumeration.fields[i];

        // TODO send patch to LLVM to support APInt in createEnumerator instead of int64_t
        // http://lists.llvm.org/pipermail/llvm-dev/2017-December/119456.html
        di_enumerators[i] = ZigLLVMCreateDebugEnumerator(g->dbuilder, buf_ptr(enum_field->name),
                bigint_as_signed(&enum_field->value));
    }

    ZigType *tag_int_type = enum_type->data.enumeration.tag_int_type;
    enum_type->llvm_type = get_llvm_type(g, tag_int_type);

    // create debug type for tag
    uint64_t tag_debug_size_in_bits = tag_int_type->size_in_bits;
    uint64_t tag_debug_align_in_bits = 8*tag_int_type->abi_align;
    ZigLLVMDIType *tag_di_type = ZigLLVMCreateDebugEnumerationType(g->dbuilder,
            ZigLLVMFileToScope(import->data.structure.root_struct->di_file), buf_ptr(&enum_type->name),
            import->data.structure.root_struct->di_file, (unsigned)(decl_node->line + 1),
            tag_debug_size_in_bits,
            tag_debug_align_in_bits,
            di_enumerators, field_count,
            get_llvm_di_type(g, tag_int_type), "");

    enum_type->llvm_di_type = tag_di_type;
    enum_type->data.enumeration.resolve_status = ResolveStatusLLVMFull;
}

static void resolve_llvm_types_union(CodeGen *g, ZigType *union_type, ResolveStatus wanted_resolve_status) {
    if (union_type->data.unionation.resolve_status >= wanted_resolve_status) return;

    bool packed = (union_type->data.unionation.layout == ContainerLayoutPacked);
    Scope *scope = &union_type->data.unionation.decls_scope->base;
    ZigType *import = get_scope_import(scope);

    TypeUnionField *most_aligned_union_member = union_type->data.unionation.most_aligned_union_member;
    ZigType *tag_type = union_type->data.unionation.tag_type;
    uint32_t gen_field_count = union_type->data.unionation.gen_field_count;
    if (gen_field_count == 0) {
        if (tag_type == nullptr) {
            union_type->llvm_type = g->builtin_types.entry_void->llvm_type;
            union_type->llvm_di_type = make_empty_namespace_llvm_di_type(g, import, buf_ptr(&union_type->name),
                    union_type->data.unionation.decl_node);
        } else {
            union_type->llvm_type = get_llvm_type(g, tag_type);
            union_type->llvm_di_type = get_llvm_di_type(g, tag_type);
        }
        union_type->data.unionation.resolve_status = ResolveStatusLLVMFull;
        return;
    }

    AstNode *decl_node = union_type->data.unionation.decl_node;

    if (union_type->data.unionation.resolve_status < ResolveStatusLLVMFwdDecl) {
        union_type->llvm_type = LLVMStructCreateNamed(LLVMGetGlobalContext(), buf_ptr(&union_type->name));
        size_t line = decl_node ? decl_node->line : 0;
        unsigned dwarf_kind = ZigLLVMTag_DW_structure_type();
        union_type->llvm_di_type = ZigLLVMCreateReplaceableCompositeType(g->dbuilder,
            dwarf_kind, buf_ptr(&union_type->name),
            ZigLLVMFileToScope(import->data.structure.root_struct->di_file),
            import->data.structure.root_struct->di_file, (unsigned)(line + 1));

        union_type->data.unionation.resolve_status = ResolveStatusLLVMFwdDecl;
        if (ResolveStatusLLVMFwdDecl >= wanted_resolve_status) return;
    }

    ZigLLVMDIType **union_inner_di_types = allocate<ZigLLVMDIType*>(gen_field_count);
    uint32_t field_count = union_type->data.unionation.src_field_count;
    for (uint32_t i = 0; i < field_count; i += 1) {
        TypeUnionField *union_field = &union_type->data.unionation.fields[i];
        if (!type_has_bits(union_field->type_entry))
            continue;

        ZigLLVMDIType *field_di_type = get_llvm_di_type(g, union_field->type_entry);
        if (union_type->data.unionation.resolve_status >= wanted_resolve_status) return;

        uint64_t store_size_in_bits = union_field->type_entry->size_in_bits;
        uint64_t abi_align_in_bits = 8*union_field->type_entry->abi_align;
        AstNode *field_node = decl_node->data.container_decl.fields.at(i);
        union_inner_di_types[union_field->gen_index] = ZigLLVMCreateDebugMemberType(g->dbuilder,
                ZigLLVMTypeToScope(union_type->llvm_di_type), buf_ptr(union_field->enum_field->name),
                import->data.structure.root_struct->di_file, (unsigned)(field_node->line + 1),
                store_size_in_bits,
                abi_align_in_bits,
                0,
                ZigLLVM_DIFlags_Zero, field_di_type);

    }

    if (tag_type == nullptr || !type_has_bits(tag_type)) {
        assert(most_aligned_union_member != nullptr);

        size_t padding_bytes = union_type->data.unionation.union_abi_size - most_aligned_union_member->type_entry->abi_size;
        if (padding_bytes > 0) {
            ZigType *u8_type = get_int_type(g, false, 8);
            ZigType *padding_array = get_array_type(g, u8_type, padding_bytes);
            LLVMTypeRef union_element_types[] = {
                most_aligned_union_member->type_entry->llvm_type,
                get_llvm_type(g, padding_array),
            };
            LLVMStructSetBody(union_type->llvm_type, union_element_types, 2, packed);
        } else {
            LLVMStructSetBody(union_type->llvm_type, &most_aligned_union_member->type_entry->llvm_type, 1, packed);
        }
        union_type->data.unionation.union_llvm_type = union_type->llvm_type;
        union_type->data.unionation.gen_tag_index = SIZE_MAX;
        union_type->data.unionation.gen_union_index = SIZE_MAX;

        // create debug type for union
        ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugUnionType(g->dbuilder,
            ZigLLVMFileToScope(import->data.structure.root_struct->di_file), buf_ptr(&union_type->name),
            import->data.structure.root_struct->di_file, (unsigned)(decl_node->line + 1),
            union_type->data.unionation.union_abi_size * 8,
            most_aligned_union_member->align * 8,
            ZigLLVM_DIFlags_Zero, union_inner_di_types,
            gen_field_count, 0, "");

        ZigLLVMReplaceTemporary(g->dbuilder, union_type->llvm_di_type, replacement_di_type);
        union_type->llvm_di_type = replacement_di_type;
        union_type->data.unionation.resolve_status = ResolveStatusLLVMFull;
        return;
    }

    LLVMTypeRef union_type_ref;
    size_t padding_bytes = union_type->data.unionation.union_abi_size - most_aligned_union_member->type_entry->abi_size;
    if (padding_bytes == 0) {
        union_type_ref = get_llvm_type(g, most_aligned_union_member->type_entry);
    } else {
        ZigType *u8_type = get_int_type(g, false, 8);
        ZigType *padding_array = get_array_type(g, u8_type, padding_bytes);
        LLVMTypeRef union_element_types[] = {
            get_llvm_type(g, most_aligned_union_member->type_entry),
            get_llvm_type(g, padding_array),
        };
        union_type_ref = LLVMStructType(union_element_types, 2, false);
    }
    union_type->data.unionation.union_llvm_type = union_type_ref;

    LLVMTypeRef root_struct_element_types[2];
    root_struct_element_types[union_type->data.unionation.gen_tag_index] = get_llvm_type(g, tag_type);
    root_struct_element_types[union_type->data.unionation.gen_union_index] = union_type_ref;
    LLVMStructSetBody(union_type->llvm_type, root_struct_element_types, 2, packed);

    // create debug type for union
    ZigLLVMDIType *union_di_type = ZigLLVMCreateDebugUnionType(g->dbuilder,
            ZigLLVMTypeToScope(union_type->llvm_di_type), "AnonUnion",
            import->data.structure.root_struct->di_file, (unsigned)(decl_node->line + 1),
            most_aligned_union_member->type_entry->size_in_bits, 8*most_aligned_union_member->align,
            ZigLLVM_DIFlags_Zero, union_inner_di_types, gen_field_count, 0, "");

    uint64_t union_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, union_type->llvm_type,
            union_type->data.unionation.gen_union_index);
    uint64_t tag_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, union_type->llvm_type,
            union_type->data.unionation.gen_tag_index);

    ZigLLVMDIType *union_member_di_type = ZigLLVMCreateDebugMemberType(g->dbuilder,
            ZigLLVMTypeToScope(union_type->llvm_di_type), "payload",
            import->data.structure.root_struct->di_file, (unsigned)(decl_node->line + 1),
            most_aligned_union_member->type_entry->size_in_bits,
            8*most_aligned_union_member->align,
            union_offset_in_bits,
            ZigLLVM_DIFlags_Zero, union_di_type);

    uint64_t tag_debug_size_in_bits = tag_type->size_in_bits;
    uint64_t tag_debug_align_in_bits = 8*tag_type->abi_align;

    ZigLLVMDIType *tag_member_di_type = ZigLLVMCreateDebugMemberType(g->dbuilder,
            ZigLLVMTypeToScope(union_type->llvm_di_type), "tag",
            import->data.structure.root_struct->di_file, (unsigned)(decl_node->line + 1),
            tag_debug_size_in_bits,
            tag_debug_align_in_bits,
            tag_offset_in_bits,
            ZigLLVM_DIFlags_Zero, get_llvm_di_type(g, tag_type));

    ZigLLVMDIType *di_root_members[2];
    di_root_members[union_type->data.unionation.gen_tag_index] = tag_member_di_type;
    di_root_members[union_type->data.unionation.gen_union_index] = union_member_di_type;

    uint64_t debug_size_in_bits = union_type->size_in_bits;
    uint64_t debug_align_in_bits = 8*union_type->abi_align;
    ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
            ZigLLVMFileToScope(import->data.structure.root_struct->di_file),
            buf_ptr(&union_type->name),
            import->data.structure.root_struct->di_file, (unsigned)(decl_node->line + 1),
            debug_size_in_bits,
            debug_align_in_bits,
            ZigLLVM_DIFlags_Zero, nullptr, di_root_members, 2, 0, nullptr, "");

    ZigLLVMReplaceTemporary(g->dbuilder, union_type->llvm_di_type, replacement_di_type);
    union_type->llvm_di_type = replacement_di_type;
    union_type->data.unionation.resolve_status = ResolveStatusLLVMFull;
}

static void resolve_llvm_types_pointer(CodeGen *g, ZigType *type, ResolveStatus wanted_resolve_status) {
    if (type->llvm_di_type != nullptr) return;

    if (resolve_pointer_zero_bits(g, type) != ErrorNone)
        zig_unreachable();

    if (!type_has_bits(type)) {
        type->llvm_type = g->builtin_types.entry_void->llvm_type;
        type->llvm_di_type = g->builtin_types.entry_void->llvm_di_type;
        return;
    }

    ZigType *elem_type = type->data.pointer.child_type;

    if (type->data.pointer.is_const || type->data.pointer.is_volatile ||
        type->data.pointer.explicit_alignment != 0 || type->data.pointer.ptr_len != PtrLenSingle ||
        type->data.pointer.bit_offset_in_host != 0 || type->data.pointer.allow_zero)
    {
        assertNoError(type_resolve(g, elem_type, ResolveStatusLLVMFwdDecl));
        ZigType *peer_type = get_pointer_to_type_extra(g, elem_type, false, false,
                PtrLenSingle, 0, 0, type->data.pointer.host_int_bytes, false);
        type->llvm_type = get_llvm_type(g, peer_type);
        type->llvm_di_type = get_llvm_di_type(g, peer_type);
        assertNoError(type_resolve(g, elem_type, wanted_resolve_status));
        return;
    }

    if (type->data.pointer.host_int_bytes == 0) {
        assertNoError(type_resolve(g, elem_type, ResolveStatusLLVMFwdDecl));
        type->llvm_type = LLVMPointerType(elem_type->llvm_type, 0);
        uint64_t debug_size_in_bits = 8*get_store_size_bytes(type->size_in_bits);
        uint64_t debug_align_in_bits = 8*type->abi_align;
        type->llvm_di_type = ZigLLVMCreateDebugPointerType(g->dbuilder, elem_type->llvm_di_type,
                debug_size_in_bits, debug_align_in_bits, buf_ptr(&type->name));
        assertNoError(type_resolve(g, elem_type, wanted_resolve_status));
    } else {
        ZigType *host_int_type = get_int_type(g, false, type->data.pointer.host_int_bytes * 8);
        LLVMTypeRef host_int_llvm_type = get_llvm_type(g, host_int_type);
        type->llvm_type = LLVMPointerType(host_int_llvm_type, 0);
        uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, host_int_llvm_type);
        uint64_t debug_align_in_bits = 8*LLVMABIAlignmentOfType(g->target_data_ref, host_int_llvm_type);
        type->llvm_di_type = ZigLLVMCreateDebugPointerType(g->dbuilder, get_llvm_di_type(g, host_int_type),
                debug_size_in_bits, debug_align_in_bits, buf_ptr(&type->name));
    }
}

static void resolve_llvm_types_integer(CodeGen *g, ZigType *type) {
    if (type->llvm_di_type != nullptr) return;

    if (!type_has_bits(type)) {
        type->llvm_type = g->builtin_types.entry_void->llvm_type;
        type->llvm_di_type = g->builtin_types.entry_void->llvm_di_type;
        return;
    }

    unsigned dwarf_tag;
    if (type->data.integral.is_signed) {
        if (type->size_in_bits == 8) {
            dwarf_tag = ZigLLVMEncoding_DW_ATE_signed_char();
        } else {
            dwarf_tag = ZigLLVMEncoding_DW_ATE_signed();
        }
    } else {
        if (type->size_in_bits == 8) {
            dwarf_tag = ZigLLVMEncoding_DW_ATE_unsigned_char();
        } else {
            dwarf_tag = ZigLLVMEncoding_DW_ATE_unsigned();
        }
    }

    type->llvm_di_type = ZigLLVMCreateDebugBasicType(g->dbuilder, buf_ptr(&type->name),
            type->abi_size * 8, dwarf_tag);
    type->llvm_type = LLVMIntType(type->size_in_bits);
}

static void resolve_llvm_types_optional(CodeGen *g, ZigType *type, ResolveStatus wanted_resolve_status) {
    assert(type->id == ZigTypeIdOptional);
    assert(type->data.maybe.resolve_status != ResolveStatusInvalid);
    assert(type->data.maybe.resolve_status >= ResolveStatusSizeKnown);
    if (type->data.maybe.resolve_status >= wanted_resolve_status) return;

    LLVMTypeRef bool_llvm_type = get_llvm_type(g, g->builtin_types.entry_bool);
    ZigLLVMDIType *bool_llvm_di_type = get_llvm_di_type(g, g->builtin_types.entry_bool);

    ZigType *child_type = type->data.maybe.child_type;
    if (!type_has_bits(child_type)) {
        type->llvm_type = bool_llvm_type;
        type->llvm_di_type = bool_llvm_di_type;
        type->data.maybe.resolve_status = ResolveStatusLLVMFull;
        return;
    }

    if (type_is_nonnull_ptr(child_type) || child_type->id == ZigTypeIdErrorSet) {
        type->llvm_type = get_llvm_type(g, child_type);
        type->llvm_di_type = get_llvm_di_type(g, child_type);
        type->data.maybe.resolve_status = ResolveStatusLLVMFull;
        return;
    }

    ZigLLVMDIScope *compile_unit_scope = ZigLLVMCompileUnitToScope(g->compile_unit);
    ZigLLVMDIFile *di_file = nullptr;
    unsigned line = 0;

    if (type->data.maybe.resolve_status < ResolveStatusLLVMFwdDecl) {
        type->llvm_type = LLVMStructCreateNamed(LLVMGetGlobalContext(), buf_ptr(&type->name));
        unsigned dwarf_kind = ZigLLVMTag_DW_structure_type();
        type->llvm_di_type = ZigLLVMCreateReplaceableCompositeType(g->dbuilder,
            dwarf_kind, buf_ptr(&type->name),
            compile_unit_scope, di_file, line);

        type->data.maybe.resolve_status = ResolveStatusLLVMFwdDecl;
        if (ResolveStatusLLVMFwdDecl >= wanted_resolve_status) return;
    }

    LLVMTypeRef child_llvm_type = get_llvm_type(g, child_type);
    ZigLLVMDIType *child_llvm_di_type = get_llvm_di_type(g, child_type);
    if (type->data.maybe.resolve_status >= wanted_resolve_status) return;

    LLVMTypeRef elem_types[] = {
        get_llvm_type(g, child_type),
        LLVMInt1Type(),
    };
    LLVMStructSetBody(type->llvm_type, elem_types, 2, false);

    uint64_t val_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, child_llvm_type);
    uint64_t val_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, child_llvm_type);
    uint64_t val_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, type->llvm_type, 0);

    uint64_t maybe_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, bool_llvm_type);
    uint64_t maybe_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, bool_llvm_type);
    uint64_t maybe_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, type->llvm_type, 1);

    uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, type->llvm_type);
    uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, type->llvm_type);

    ZigLLVMDIType *di_element_types[] = {
        ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(type->llvm_di_type),
                "val", di_file, line,
                val_debug_size_in_bits,
                val_debug_align_in_bits,
                val_offset_in_bits,
                ZigLLVM_DIFlags_Zero, child_llvm_di_type),
        ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(type->llvm_di_type),
                "maybe", di_file, line,
                maybe_debug_size_in_bits,
                maybe_debug_align_in_bits,
                maybe_offset_in_bits,
                ZigLLVM_DIFlags_Zero, bool_llvm_di_type),
    };
    ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
            compile_unit_scope,
            buf_ptr(&type->name),
            di_file, line, debug_size_in_bits, debug_align_in_bits, ZigLLVM_DIFlags_Zero,
            nullptr, di_element_types, 2, 0, nullptr, "");

    ZigLLVMReplaceTemporary(g->dbuilder, type->llvm_di_type, replacement_di_type);
    type->llvm_di_type = replacement_di_type;
    type->data.maybe.resolve_status = ResolveStatusLLVMFull;
}

static void resolve_llvm_types_error_union(CodeGen *g, ZigType *type) {
    if (type->llvm_di_type != nullptr) return;

    ZigType *payload_type = type->data.error_union.payload_type;
    ZigType *err_set_type = type->data.error_union.err_set_type;

    if (!type_has_bits(payload_type)) {
        assert(type_has_bits(err_set_type));
        type->llvm_type = get_llvm_type(g, err_set_type);
        type->llvm_di_type = get_llvm_di_type(g, err_set_type);
    } else if (!type_has_bits(err_set_type)) {
        type->llvm_type = get_llvm_type(g, payload_type);
        type->llvm_di_type = get_llvm_di_type(g, payload_type);
    } else {
        LLVMTypeRef err_set_llvm_type = get_llvm_type(g, err_set_type);
        LLVMTypeRef payload_llvm_type = get_llvm_type(g, payload_type);
        LLVMTypeRef elem_types[3];
        elem_types[err_union_err_index] = err_set_llvm_type;
        elem_types[err_union_payload_index] = payload_llvm_type;

        type->llvm_type = LLVMStructType(elem_types, 2, false);
        if (LLVMABISizeOfType(g->target_data_ref, type->llvm_type) != type->abi_size) {
            // we need to do our own padding
            type->data.error_union.pad_llvm_type = LLVMArrayType(LLVMInt8Type(), type->data.error_union.pad_bytes);
            elem_types[2] = type->data.error_union.pad_llvm_type;
            type->llvm_type = LLVMStructType(elem_types, 3, false);
        }

        ZigLLVMDIScope *compile_unit_scope = ZigLLVMCompileUnitToScope(g->compile_unit);
        ZigLLVMDIFile *di_file = nullptr;
        unsigned line = 0;
        type->llvm_di_type = ZigLLVMCreateReplaceableCompositeType(g->dbuilder,
            ZigLLVMTag_DW_structure_type(), buf_ptr(&type->name),
            compile_unit_scope, di_file, line);

        uint64_t tag_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, err_set_llvm_type);
        uint64_t tag_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, err_set_llvm_type);
        uint64_t tag_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, type->llvm_type, err_union_err_index);

        uint64_t value_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, payload_llvm_type);
        uint64_t value_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, payload_llvm_type);
        uint64_t value_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, type->llvm_type,
                err_union_payload_index);

        uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, type->llvm_type);
        uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, type->llvm_type);

        ZigLLVMDIType *di_element_types[2];
        di_element_types[err_union_err_index] = ZigLLVMCreateDebugMemberType(g->dbuilder,
                ZigLLVMTypeToScope(type->llvm_di_type),
                    "tag", di_file, line,
                    tag_debug_size_in_bits,
                    tag_debug_align_in_bits,
                    tag_offset_in_bits,
                    ZigLLVM_DIFlags_Zero, get_llvm_di_type(g, err_set_type));
        di_element_types[err_union_payload_index] = ZigLLVMCreateDebugMemberType(g->dbuilder,
                ZigLLVMTypeToScope(type->llvm_di_type),
                    "value", di_file, line,
                    value_debug_size_in_bits,
                    value_debug_align_in_bits,
                    value_offset_in_bits,
                    ZigLLVM_DIFlags_Zero, get_llvm_di_type(g, payload_type));

        ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
                compile_unit_scope,
                buf_ptr(&type->name),
                di_file, line,
                debug_size_in_bits,
                debug_align_in_bits,
                ZigLLVM_DIFlags_Zero,
                nullptr, di_element_types, 2, 0, nullptr, "");

        ZigLLVMReplaceTemporary(g->dbuilder, type->llvm_di_type, replacement_di_type);
        type->llvm_di_type = replacement_di_type;
    }
}

static void resolve_llvm_types_array(CodeGen *g, ZigType *type) {
    if (type->llvm_di_type != nullptr) return;

    if (!type_has_bits(type)) {
        type->llvm_type = g->builtin_types.entry_void->llvm_type;
        type->llvm_di_type = g->builtin_types.entry_void->llvm_di_type;
        return;
    }

    ZigType *elem_type = type->data.array.child_type;

    // TODO https://github.com/ziglang/zig/issues/1424
    type->llvm_type = LLVMArrayType(get_llvm_type(g, elem_type), (unsigned)type->data.array.len);

    uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, type->llvm_type);
    uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, type->llvm_type);

    type->llvm_di_type = ZigLLVMCreateDebugArrayType(g->dbuilder, debug_size_in_bits,
            debug_align_in_bits, get_llvm_di_type(g, elem_type), (int)type->data.array.len);
}

static void resolve_llvm_types_fn_type(CodeGen *g, ZigType *fn_type) {
    if (fn_type->llvm_di_type != nullptr) return;

    FnTypeId *fn_type_id = &fn_type->data.fn.fn_type_id;
    bool first_arg_return = want_first_arg_sret(g, fn_type_id);
    bool is_async = fn_type_id->cc == CallingConventionAsync;
    bool is_c_abi = fn_type_id->cc == CallingConventionC;
    bool prefix_arg_error_return_trace = g->have_err_ret_tracing && fn_type_can_fail(fn_type_id);
    // +1 for maybe making the first argument the return value
    // +1 for maybe first argument the error return trace
    // +2 for maybe arguments async allocator and error code pointer
    ZigList<LLVMTypeRef> gen_param_types = {};
    // +1 because 0 is the return type and
    // +1 for maybe making first arg ret val and
    // +1 for maybe first argument the error return trace
    // +2 for maybe arguments async allocator and error code pointer
    ZigList<ZigLLVMDIType *> param_di_types = {};
    ZigType *gen_return_type;
    if (is_async) {
        gen_return_type = g->builtin_types.entry_void;
        param_di_types.append(get_llvm_di_type(g, gen_return_type));
    } else if (!type_has_bits(fn_type_id->return_type)) {
        gen_return_type = g->builtin_types.entry_void;
        param_di_types.append(get_llvm_di_type(g, gen_return_type));
    } else if (first_arg_return) {
        gen_return_type = g->builtin_types.entry_void;
        param_di_types.append(get_llvm_di_type(g, gen_return_type));
        ZigType *gen_type = get_pointer_to_type(g, fn_type_id->return_type, false);
        gen_param_types.append(get_llvm_type(g, gen_type));
        param_di_types.append(get_llvm_di_type(g, gen_type));
    } else {
        gen_return_type = fn_type_id->return_type;
        param_di_types.append(get_llvm_di_type(g, gen_return_type));
    }
    fn_type->data.fn.gen_return_type = gen_return_type;

    if (prefix_arg_error_return_trace && !is_async) {
        ZigType *gen_type = get_pointer_to_type(g, get_stack_trace_type(g), false);
        gen_param_types.append(get_llvm_type(g, gen_type));
        param_di_types.append(get_llvm_di_type(g, gen_type));
    }
    if (is_async) {
        fn_type->data.fn.gen_param_info = allocate<FnGenParamInfo>(2);

        ZigType *frame_type = get_any_frame_type(g, fn_type_id->return_type);
        gen_param_types.append(get_llvm_type(g, frame_type));
        param_di_types.append(get_llvm_di_type(g, frame_type));

        fn_type->data.fn.gen_param_info[0].src_index = 0;
        fn_type->data.fn.gen_param_info[0].gen_index = 0;
        fn_type->data.fn.gen_param_info[0].type = frame_type;

        gen_param_types.append(get_llvm_type(g, g->builtin_types.entry_usize));
        param_di_types.append(get_llvm_di_type(g, g->builtin_types.entry_usize));

        fn_type->data.fn.gen_param_info[1].src_index = 1;
        fn_type->data.fn.gen_param_info[1].gen_index = 1;
        fn_type->data.fn.gen_param_info[1].type = g->builtin_types.entry_usize;
    } else {
        fn_type->data.fn.gen_param_info = allocate<FnGenParamInfo>(fn_type_id->param_count);
        for (size_t i = 0; i < fn_type_id->param_count; i += 1) {
            FnTypeParamInfo *src_param_info = &fn_type->data.fn.fn_type_id.param_info[i];
            ZigType *type_entry = src_param_info->type;
            FnGenParamInfo *gen_param_info = &fn_type->data.fn.gen_param_info[i];

            gen_param_info->src_index = i;
            gen_param_info->gen_index = SIZE_MAX;

            if (is_c_abi || !type_has_bits(type_entry))
                continue;

            ZigType *gen_type;
            if (handle_is_ptr(type_entry)) {
                gen_type = get_pointer_to_type(g, type_entry, true);
                gen_param_info->is_byval = true;
            } else {
                gen_type = type_entry;
            }
            gen_param_info->gen_index = gen_param_types.length;
            gen_param_info->type = gen_type;
            gen_param_types.append(get_llvm_type(g, gen_type));

            param_di_types.append(get_llvm_di_type(g, gen_type));
        }
    }

    if (is_c_abi) {
        FnWalk fn_walk = {};
        fn_walk.id = FnWalkIdTypes;
        fn_walk.data.types.param_di_types = &param_di_types;
        fn_walk.data.types.gen_param_types = &gen_param_types;
        walk_function_params(g, fn_type, &fn_walk);
    }

    fn_type->data.fn.gen_param_count = gen_param_types.length;

    for (size_t i = 0; i < gen_param_types.length; i += 1) {
        assert(gen_param_types.items[i] != nullptr);
    }

    fn_type->data.fn.raw_type_ref = LLVMFunctionType(get_llvm_type(g, gen_return_type),
            gen_param_types.items, (unsigned int)gen_param_types.length, fn_type_id->is_var_args);
    fn_type->llvm_type = LLVMPointerType(fn_type->data.fn.raw_type_ref, 0);
    fn_type->data.fn.raw_di_type = ZigLLVMCreateSubroutineType(g->dbuilder, param_di_types.items, (int)param_di_types.length, 0);
    fn_type->llvm_di_type = ZigLLVMCreateDebugPointerType(g->dbuilder, fn_type->data.fn.raw_di_type,
            LLVMStoreSizeOfType(g->target_data_ref, fn_type->llvm_type),
            LLVMABIAlignmentOfType(g->target_data_ref, fn_type->llvm_type), "");

    gen_param_types.deinit();
    param_di_types.deinit();
}

void resolve_llvm_types_fn(CodeGen *g, ZigFn *fn) {
    Error err;
    if (fn->raw_di_type != nullptr) return;

    ZigType *fn_type = fn->type_entry;
    if (!fn_is_async(fn)) {
        resolve_llvm_types_fn_type(g, fn_type);
        fn->raw_type_ref = fn_type->data.fn.raw_type_ref;
        fn->raw_di_type = fn_type->data.fn.raw_di_type;
        return;
    }

    ZigType *gen_return_type = g->builtin_types.entry_void;
    ZigList<ZigLLVMDIType *> param_di_types = {};
    ZigList<LLVMTypeRef> gen_param_types = {};
    // first "parameter" is return value
    param_di_types.append(get_llvm_di_type(g, gen_return_type));

    ZigType *frame_type = get_fn_frame_type(g, fn);
    ZigType *ptr_type = get_pointer_to_type(g, frame_type, false);
    if ((err = type_resolve(g, ptr_type, ResolveStatusLLVMFwdDecl)))
        zig_unreachable();
    gen_param_types.append(ptr_type->llvm_type);
    param_di_types.append(ptr_type->llvm_di_type);

    // this parameter is used to pass the result pointer when await completes
    gen_param_types.append(get_llvm_type(g, g->builtin_types.entry_usize));
    param_di_types.append(get_llvm_di_type(g, g->builtin_types.entry_usize));

    fn->raw_type_ref = LLVMFunctionType(get_llvm_type(g, gen_return_type),
            gen_param_types.items, gen_param_types.length, false);
    fn->raw_di_type = ZigLLVMCreateSubroutineType(g->dbuilder, param_di_types.items, (int)param_di_types.length, 0);

    param_di_types.deinit();
    gen_param_types.deinit();
}

static void resolve_llvm_types_anyerror(CodeGen *g) {
    ZigType *entry = g->builtin_types.entry_global_error_set;
    entry->llvm_type = get_llvm_type(g, g->err_tag_type);
    ZigList<ZigLLVMDIEnumerator *> err_enumerators = {};
    // reserve index 0 to indicate no error
    err_enumerators.append(ZigLLVMCreateDebugEnumerator(g->dbuilder, "(none)", 0));
    for (size_t i = 1; i < g->errors_by_index.length; i += 1) {
        ErrorTableEntry *error_entry = g->errors_by_index.at(i);
        err_enumerators.append(ZigLLVMCreateDebugEnumerator(g->dbuilder, buf_ptr(&error_entry->name), i));
    }

    // create debug type for error sets
    uint64_t tag_debug_size_in_bits = g->err_tag_type->size_in_bits;
    uint64_t tag_debug_align_in_bits = 8*g->err_tag_type->abi_align;
    ZigLLVMDIFile *err_set_di_file = nullptr;
    entry->llvm_di_type = ZigLLVMCreateDebugEnumerationType(g->dbuilder,
            ZigLLVMCompileUnitToScope(g->compile_unit), buf_ptr(&entry->name),
            err_set_di_file, 0,
            tag_debug_size_in_bits,
            tag_debug_align_in_bits,
            err_enumerators.items, err_enumerators.length,
            get_llvm_di_type(g, g->err_tag_type), "");

    err_enumerators.deinit();
}

static void resolve_llvm_types_async_frame(CodeGen *g, ZigType *frame_type, ResolveStatus wanted_resolve_status) {
    Error err;
    if ((err = type_resolve(g, frame_type, ResolveStatusSizeKnown)))
        zig_unreachable();

    ZigType *passed_frame_type = fn_is_async(frame_type->data.frame.fn) ? frame_type : nullptr;
    resolve_llvm_types_struct(g, frame_type->data.frame.locals_struct, wanted_resolve_status, passed_frame_type);
    frame_type->llvm_type = frame_type->data.frame.locals_struct->llvm_type;
    frame_type->llvm_di_type = frame_type->data.frame.locals_struct->llvm_di_type;
}

static void resolve_llvm_types_any_frame(CodeGen *g, ZigType *any_frame_type, ResolveStatus wanted_resolve_status) {
    if (any_frame_type->llvm_di_type != nullptr) return;

    Buf *name = buf_sprintf("(%s header)", buf_ptr(&any_frame_type->name));
    LLVMTypeRef frame_header_type = LLVMStructCreateNamed(LLVMGetGlobalContext(), buf_ptr(name));
    any_frame_type->llvm_type = LLVMPointerType(frame_header_type, 0);

    unsigned dwarf_kind = ZigLLVMTag_DW_structure_type();
    ZigLLVMDIFile *di_file = nullptr;
    ZigLLVMDIScope *di_scope = ZigLLVMCompileUnitToScope(g->compile_unit);
    unsigned line = 0;
    ZigLLVMDIType *frame_header_di_type = ZigLLVMCreateReplaceableCompositeType(g->dbuilder,
        dwarf_kind, buf_ptr(name), di_scope, di_file, line);
    any_frame_type->llvm_di_type = ZigLLVMCreateDebugPointerType(g->dbuilder, frame_header_di_type,
            8*g->pointer_size_bytes, 8*g->builtin_types.entry_usize->abi_align, buf_ptr(&any_frame_type->name));

    LLVMTypeRef llvm_void = LLVMVoidType();
    LLVMTypeRef arg_types[] = {any_frame_type->llvm_type, g->builtin_types.entry_usize->llvm_type};
    LLVMTypeRef fn_type = LLVMFunctionType(llvm_void, arg_types, 2, false);
    LLVMTypeRef usize_type_ref = get_llvm_type(g, g->builtin_types.entry_usize);
    ZigLLVMDIType *usize_di_type = get_llvm_di_type(g, g->builtin_types.entry_usize);
    ZigLLVMDIScope *compile_unit_scope = ZigLLVMCompileUnitToScope(g->compile_unit);

    ZigType *result_type = any_frame_type->data.any_frame.result_type;
    ZigType *ptr_result_type = (result_type == nullptr) ? nullptr : get_pointer_to_type(g, result_type, false);
    LLVMTypeRef ptr_fn_llvm_type = LLVMPointerType(fn_type, 0);
    if (result_type == nullptr) {
        g->anyframe_fn_type = ptr_fn_llvm_type;
    }

    ZigList<LLVMTypeRef> field_types = {};
    ZigList<ZigLLVMDIType *> di_element_types = {};

    // label (grep this): [fn_frame_struct_layout]
    field_types.append(ptr_fn_llvm_type); // fn_ptr
    field_types.append(usize_type_ref); // resume_index
    field_types.append(usize_type_ref); // awaiter

    bool have_result_type = result_type != nullptr && type_has_bits(result_type);
    if (have_result_type) {
        field_types.append(get_llvm_type(g, ptr_result_type)); // result_ptr_callee
        field_types.append(get_llvm_type(g, ptr_result_type)); // result_ptr_awaiter
        field_types.append(get_llvm_type(g, result_type)); // result
        if (codegen_fn_has_err_ret_tracing_arg(g, result_type)) {
            ZigType *ptr_stack_trace = get_pointer_to_type(g, get_stack_trace_type(g), false);
            field_types.append(get_llvm_type(g, ptr_stack_trace)); // ptr_stack_trace_callee
            field_types.append(get_llvm_type(g, ptr_stack_trace)); // ptr_stack_trace_awaiter
        }
    }
    LLVMStructSetBody(frame_header_type, field_types.items, field_types.length, false);

    di_element_types.append(
        ZigLLVMCreateDebugMemberType(g->dbuilder,
            ZigLLVMTypeToScope(any_frame_type->llvm_di_type), "fn_ptr",
            di_file, line,
            8*LLVMABISizeOfType(g->target_data_ref, field_types.at(di_element_types.length)),
            8*LLVMABIAlignmentOfType(g->target_data_ref, field_types.at(di_element_types.length)),
            8*LLVMOffsetOfElement(g->target_data_ref, frame_header_type, di_element_types.length),
            ZigLLVM_DIFlags_Zero, usize_di_type));
    di_element_types.append(
        ZigLLVMCreateDebugMemberType(g->dbuilder,
            ZigLLVMTypeToScope(any_frame_type->llvm_di_type), "resume_index",
            di_file, line,
            8*LLVMABISizeOfType(g->target_data_ref, field_types.at(di_element_types.length)),
            8*LLVMABIAlignmentOfType(g->target_data_ref, field_types.at(di_element_types.length)),
            8*LLVMOffsetOfElement(g->target_data_ref, frame_header_type, di_element_types.length),
            ZigLLVM_DIFlags_Zero, usize_di_type));
    di_element_types.append(
        ZigLLVMCreateDebugMemberType(g->dbuilder,
            ZigLLVMTypeToScope(any_frame_type->llvm_di_type), "awaiter",
            di_file, line,
            8*LLVMABISizeOfType(g->target_data_ref, field_types.at(di_element_types.length)),
            8*LLVMABIAlignmentOfType(g->target_data_ref, field_types.at(di_element_types.length)),
            8*LLVMOffsetOfElement(g->target_data_ref, frame_header_type, di_element_types.length),
            ZigLLVM_DIFlags_Zero, usize_di_type));

    if (have_result_type) {
        di_element_types.append(
            ZigLLVMCreateDebugMemberType(g->dbuilder,
                ZigLLVMTypeToScope(any_frame_type->llvm_di_type), "result_ptr_callee",
                di_file, line,
                8*LLVMABISizeOfType(g->target_data_ref, field_types.at(di_element_types.length)),
                8*LLVMABIAlignmentOfType(g->target_data_ref, field_types.at(di_element_types.length)),
                8*LLVMOffsetOfElement(g->target_data_ref, frame_header_type, di_element_types.length),
                ZigLLVM_DIFlags_Zero, get_llvm_di_type(g, ptr_result_type)));
        di_element_types.append(
            ZigLLVMCreateDebugMemberType(g->dbuilder,
                ZigLLVMTypeToScope(any_frame_type->llvm_di_type), "result_ptr_awaiter",
                di_file, line,
                8*LLVMABISizeOfType(g->target_data_ref, field_types.at(di_element_types.length)),
                8*LLVMABIAlignmentOfType(g->target_data_ref, field_types.at(di_element_types.length)),
                8*LLVMOffsetOfElement(g->target_data_ref, frame_header_type, di_element_types.length),
                ZigLLVM_DIFlags_Zero, get_llvm_di_type(g, ptr_result_type)));
        di_element_types.append(
            ZigLLVMCreateDebugMemberType(g->dbuilder,
                ZigLLVMTypeToScope(any_frame_type->llvm_di_type), "result",
                di_file, line,
                8*LLVMABISizeOfType(g->target_data_ref, field_types.at(di_element_types.length)),
                8*LLVMABIAlignmentOfType(g->target_data_ref, field_types.at(di_element_types.length)),
                8*LLVMOffsetOfElement(g->target_data_ref, frame_header_type, di_element_types.length),
                ZigLLVM_DIFlags_Zero, get_llvm_di_type(g, result_type)));

        if (codegen_fn_has_err_ret_tracing_arg(g, result_type)) {
            ZigType *ptr_stack_trace = get_pointer_to_type(g, get_stack_trace_type(g), false);
            di_element_types.append(
                ZigLLVMCreateDebugMemberType(g->dbuilder,
                    ZigLLVMTypeToScope(any_frame_type->llvm_di_type), "ptr_stack_trace_callee",
                    di_file, line,
                    8*LLVMABISizeOfType(g->target_data_ref, field_types.at(di_element_types.length)),
                    8*LLVMABIAlignmentOfType(g->target_data_ref, field_types.at(di_element_types.length)),
                    8*LLVMOffsetOfElement(g->target_data_ref, frame_header_type, di_element_types.length),
                    ZigLLVM_DIFlags_Zero, get_llvm_di_type(g, ptr_stack_trace)));
            di_element_types.append(
                ZigLLVMCreateDebugMemberType(g->dbuilder,
                    ZigLLVMTypeToScope(any_frame_type->llvm_di_type), "ptr_stack_trace_awaiter",
                    di_file, line,
                    8*LLVMABISizeOfType(g->target_data_ref, field_types.at(di_element_types.length)),
                    8*LLVMABIAlignmentOfType(g->target_data_ref, field_types.at(di_element_types.length)),
                    8*LLVMOffsetOfElement(g->target_data_ref, frame_header_type, di_element_types.length),
                    ZigLLVM_DIFlags_Zero, get_llvm_di_type(g, ptr_stack_trace)));
        }
    };

    ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
            compile_unit_scope, buf_ptr(name),
            di_file, line,
            8*LLVMABISizeOfType(g->target_data_ref, frame_header_type),
            8*LLVMABIAlignmentOfType(g->target_data_ref, frame_header_type),
            ZigLLVM_DIFlags_Zero,
            nullptr, di_element_types.items, di_element_types.length, 0, nullptr, "");

    ZigLLVMReplaceTemporary(g->dbuilder, frame_header_di_type, replacement_di_type);

    field_types.deinit();
    di_element_types.deinit();
}

static void resolve_llvm_types(CodeGen *g, ZigType *type, ResolveStatus wanted_resolve_status) {
    assert(wanted_resolve_status > ResolveStatusSizeKnown);
    switch (type->id) {
        case ZigTypeIdInvalid:
        case ZigTypeIdMetaType:
        case ZigTypeIdComptimeFloat:
        case ZigTypeIdComptimeInt:
        case ZigTypeIdEnumLiteral:
        case ZigTypeIdUndefined:
        case ZigTypeIdNull:
        case ZigTypeIdBoundFn:
        case ZigTypeIdArgTuple:
            zig_unreachable();
        case ZigTypeIdFloat:
        case ZigTypeIdOpaque:
        case ZigTypeIdVoid:
        case ZigTypeIdBool:
        case ZigTypeIdUnreachable:
            assert(type->llvm_di_type != nullptr);
            return;
        case ZigTypeIdStruct:
            if (type->data.structure.is_slice)
                return resolve_llvm_types_slice(g, type, wanted_resolve_status);
            else
                return resolve_llvm_types_struct(g, type, wanted_resolve_status, nullptr);
        case ZigTypeIdEnum:
            return resolve_llvm_types_enum(g, type, wanted_resolve_status);
        case ZigTypeIdUnion:
            return resolve_llvm_types_union(g, type, wanted_resolve_status);
        case ZigTypeIdPointer:
            return resolve_llvm_types_pointer(g, type, wanted_resolve_status);
        case ZigTypeIdInt:
            return resolve_llvm_types_integer(g, type);
        case ZigTypeIdOptional:
            return resolve_llvm_types_optional(g, type, wanted_resolve_status);
        case ZigTypeIdErrorUnion:
            return resolve_llvm_types_error_union(g, type);
        case ZigTypeIdArray:
            return resolve_llvm_types_array(g, type);
        case ZigTypeIdFn:
            return resolve_llvm_types_fn_type(g, type);
        case ZigTypeIdErrorSet: {
            if (type->llvm_di_type != nullptr) return;

            if (g->builtin_types.entry_global_error_set->llvm_type == nullptr) {
                resolve_llvm_types_anyerror(g);
            }
            type->llvm_type = g->builtin_types.entry_global_error_set->llvm_type;
            type->llvm_di_type = g->builtin_types.entry_global_error_set->llvm_di_type;
            return;
        }
        case ZigTypeIdVector: {
            if (type->llvm_di_type != nullptr) return;

            type->llvm_type = LLVMVectorType(get_llvm_type(g, type->data.vector.elem_type), type->data.vector.len);
            type->llvm_di_type = ZigLLVMDIBuilderCreateVectorType(g->dbuilder, type->size_in_bits,
                    type->abi_align, get_llvm_di_type(g, type->data.vector.elem_type), type->data.vector.len);
            return;
        }
        case ZigTypeIdFnFrame:
            return resolve_llvm_types_async_frame(g, type, wanted_resolve_status);
        case ZigTypeIdAnyFrame:
            return resolve_llvm_types_any_frame(g, type, wanted_resolve_status);
    }
    zig_unreachable();
}

LLVMTypeRef get_llvm_type(CodeGen *g, ZigType *type) {
    assertNoError(type_resolve(g, type, ResolveStatusLLVMFull));
    assert(type->abi_size == 0 || type->abi_size >= LLVMABISizeOfType(g->target_data_ref, type->llvm_type));
    assert(type->abi_align == 0 || type->abi_align >= LLVMABIAlignmentOfType(g->target_data_ref, type->llvm_type));
    return type->llvm_type;
}

ZigLLVMDIType *get_llvm_di_type(CodeGen *g, ZigType *type) {
    assertNoError(type_resolve(g, type, ResolveStatusLLVMFull));
    return type->llvm_di_type;
}

void src_assert(bool ok, AstNode *source_node) {
    if (ok) return;
    if (source_node == nullptr) {
        fprintf(stderr, "when analyzing (unknown source location): ");
    } else {
        fprintf(stderr, "when analyzing %s:%u:%u: ",
            buf_ptr(source_node->owner->data.structure.root_struct->path),
            (unsigned)source_node->line + 1, (unsigned)source_node->column + 1);
    }
    const char *msg = "assertion failed. This is a bug in the Zig compiler.";
    stage2_panic(msg, strlen(msg));
}

IrInstruction *ir_create_alloca(CodeGen *g, Scope *scope, AstNode *source_node, ZigFn *fn,
        ZigType *var_type, const char *name_hint)
{
    IrInstructionAllocaGen *alloca_gen = allocate<IrInstructionAllocaGen>(1);
    alloca_gen->base.id = IrInstructionIdAllocaGen;
    alloca_gen->base.source_node = source_node;
    alloca_gen->base.scope = scope;
    alloca_gen->base.value.type = get_pointer_to_type(g, var_type, false);
    alloca_gen->base.ref_count = 1;
    alloca_gen->name_hint = name_hint;
    fn->alloca_gen_list.append(alloca_gen);
    return &alloca_gen->base;
}

Error analyze_import(CodeGen *g, ZigType *source_import, Buf *import_target_str,
        ZigType **out_import, Buf **out_import_target_path, Buf *out_full_path)
{
    Error err;

    Buf *search_dir;
    ZigPackage *cur_scope_pkg = source_import->data.structure.root_struct->package;
    assert(cur_scope_pkg);
    ZigPackage *target_package;
    auto package_entry = cur_scope_pkg->package_table.maybe_get(import_target_str);
    SourceKind source_kind;
    if (package_entry) {
        target_package = package_entry->value;
        *out_import_target_path = &target_package->root_src_path;
        search_dir = &target_package->root_src_dir;
        source_kind = SourceKindPkgMain;
    } else {
        // try it as a filename
        target_package = cur_scope_pkg;
        *out_import_target_path = import_target_str;

        // search relative to importing file
        search_dir = buf_alloc();
        os_path_dirname(source_import->data.structure.root_struct->path, search_dir);

        source_kind = SourceKindNonRoot;
    }

    buf_resize(out_full_path, 0);
    os_path_join(search_dir, *out_import_target_path, out_full_path);

    Buf *import_code = buf_alloc();
    Buf *resolved_path = buf_alloc();

    Buf *resolve_paths[] = { out_full_path, };
    *resolved_path = os_path_resolve(resolve_paths, 1);

    auto import_entry = g->import_table.maybe_get(resolved_path);
    if (import_entry) {
        *out_import = import_entry->value;
        return ErrorNone;
    }

    if (source_kind == SourceKindNonRoot) {
        Buf *pkg_root_src_dir = &cur_scope_pkg->root_src_dir;
        Buf resolved_root_src_dir = os_path_resolve(&pkg_root_src_dir, 1);
        if (!buf_starts_with_buf(resolved_path, &resolved_root_src_dir)) {
            return ErrorImportOutsidePkgPath;
        }
    }

    if ((err = file_fetch(g, resolved_path, import_code))) {
        return err;
    }

    *out_import = add_source_file(g, target_package, resolved_path, import_code, source_kind);
    return ErrorNone;
}

