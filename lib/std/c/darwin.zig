const std = @import("../std.zig");
const assert = std.debug.assert;
const builtin = @import("builtin");
const macho = std.macho;

usingnamespace @import("../os/bits.zig");

extern "c" fn __error() *c_int;
pub extern "c" fn _NSGetExecutablePath(buf: [*]u8, bufsize: *u32) c_int;
pub extern "c" fn _dyld_get_image_header(image_index: u32) ?*mach_header;

pub extern "c" fn __getdirentries64(fd: c_int, buf_ptr: [*]u8, buf_len: usize, basep: *i64) isize;

pub extern "c" fn mach_absolute_time() u64;
pub extern "c" fn mach_timebase_info(tinfo: ?*mach_timebase_info_data) void;

pub extern "c" fn kevent64(
    kq: c_int,
    changelist: [*]const kevent64_s,
    nchanges: c_int,
    eventlist: [*]kevent64_s,
    nevents: c_int,
    flags: c_uint,
    timeout: ?*const timespec,
) c_int;

const mach_hdr = if (@sizeOf(usize) == 8) mach_header_64 else mach_header;

/// The value of the link editor defined symbol _MH_EXECUTE_SYM is the address
/// of the mach header in a Mach-O executable file type.  It does not appear in
/// any file type other than a MH_EXECUTE file type.  The type of the symbol is
/// absolute as the header is not part of any section.
/// This symbol is populated when linking the system's libc, which is guaranteed
/// on this operating system. However when building object files or libraries,
/// the system libc won't be linked until the final executable. So we
/// export a weak symbol here, to be overridden by the real one.
pub extern "c" var _mh_execute_header: mach_hdr = undefined;
comptime {
    if (std.Target.current.isDarwin()) {
        @export("_mh_execute_header", _mh_execute_header, .Weak);
    }
}

pub const mach_header_64 = macho.mach_header_64;
pub const mach_header = macho.mach_header;

pub const _errno = __error;

pub extern "c" fn mach_host_self() mach_port_t;
pub extern "c" fn clock_get_time(clock_serv: clock_serv_t, cur_time: *mach_timespec_t) kern_return_t;
pub extern "c" fn host_get_clock_service(host: host_t, clock_id: clock_id_t, clock_serv: ?[*]clock_serv_t) kern_return_t;
pub extern "c" fn mach_port_deallocate(task: ipc_space_t, name: mach_port_name_t) kern_return_t;

pub fn sigaddset(set: *sigset_t, signo: u5) void {
    set.* |= u32(1) << (signo - 1);
}

pub extern "c" fn sigaltstack(ss: ?*stack_t, old_ss: ?*stack_t) c_int;

/// get address to use bind()
pub const AI_PASSIVE = 0x00000001;

/// fill ai_canonname
pub const AI_CANONNAME = 0x00000002;

/// prevent host name resolution
pub const AI_NUMERICHOST = 0x00000004;

/// prevent service name resolution
pub const AI_NUMERICSERV = 0x00001000;

///  address family for hostname not supported
pub const EAI_ADDRFAMILY = 1;

/// temporary failure in name resolution
pub const EAI_AGAIN = 2;

/// invalid value for ai_flags
pub const EAI_BADFLAGS = 3;

/// non-recoverable failure in name resolution
pub const EAI_FAIL = 4;

/// ai_family not supported
pub const EAI_FAMILY = 5;

/// memory allocation failure
pub const EAI_MEMORY = 6;

/// no address associated with hostname
pub const EAI_NODATA = 7;

/// hostname nor servname provided, or not known
pub const EAI_NONAME = 8;

/// servname not supported for ai_socktype
pub const EAI_SERVICE = 9;

/// ai_socktype not supported
pub const EAI_SOCKTYPE = 10;

/// system error returned in errno
pub const EAI_SYSTEM = 11;

/// invalid value for hints
pub const EAI_BADHINTS = 12;

/// resolved protocol is unknown
pub const EAI_PROTOCOL = 13;

/// argument buffer overflow
pub const EAI_OVERFLOW = 14;
pub const EAI_MAX = 15;
