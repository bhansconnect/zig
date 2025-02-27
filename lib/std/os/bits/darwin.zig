const std = @import("../../std.zig");
const assert = std.debug.assert;
const maxInt = std.math.maxInt;

pub const fd_t = c_int;
pub const pid_t = c_int;

pub const in_port_t = u16;
pub const sa_family_t = u8;
pub const socklen_t = u32;
pub const sockaddr = extern struct {
    len: u8,
    family: sa_family_t,
    data: [14]u8,
};
pub const sockaddr_in = extern struct {
    len: u8 = @sizeOf(sockaddr_in),
    family: sa_family_t = AF_INET,
    port: in_port_t,
    addr: u32,
    zero: [8]u8 = [8]u8{ 0, 0, 0, 0, 0, 0, 0, 0 },
};
pub const sockaddr_in6 = extern struct {
    len: u8 = @sizeOf(sockaddr_in6),
    family: sa_family_t = AF_INET6,
    port: in_port_t,
    flowinfo: u32,
    addr: [16]u8,
    scope_id: u32,
};

/// UNIX domain socket
pub const sockaddr_un = extern struct {
    len: u8 = @sizeOf(sockaddr_un),
    family: sa_family_t = AF_UNIX,
    path: [104]u8,
};

pub const timeval = extern struct {
    tv_sec: c_long,
    tv_usec: i32,
};

pub const timezone = extern struct {
    tz_minuteswest: i32,
    tz_dsttime: i32,
};

pub const mach_timebase_info_data = extern struct {
    numer: u32,
    denom: u32,
};

pub const off_t = i64;

/// Renamed to Stat to not conflict with the stat function.
/// atime, mtime, and ctime have functions to return `timespec`,
/// because although this is a POSIX API, the layout and names of
/// the structs are inconsistent across operating systems, and
/// in C, macros are used to hide the differences. Here we use
/// methods to accomplish this.
pub const Stat = extern struct {
    dev: i32,
    mode: u16,
    nlink: u16,
    ino: u64,
    uid: u32,
    gid: u32,
    rdev: i32,
    atimesec: isize,
    atimensec: isize,
    mtimesec: isize,
    mtimensec: isize,
    ctimesec: isize,
    ctimensec: isize,
    birthtimesec: isize,
    birthtimensec: isize,
    size: off_t,
    blocks: i64,
    blksize: i32,
    flags: u32,
    gen: u32,
    lspare: i32,
    qspare: [2]i64,

    pub fn atime(self: Stat) timespec {
        return timespec{
            .tv_sec = self.atimesec,
            .tv_nsec = self.atimensec,
        };
    }

    pub fn mtime(self: Stat) timespec {
        return timespec{
            .tv_sec = self.mtimesec,
            .tv_nsec = self.mtimensec,
        };
    }

    pub fn ctime(self: Stat) timespec {
        return timespec{
            .tv_sec = self.ctimesec,
            .tv_nsec = self.ctimensec,
        };
    }
};

pub const timespec = extern struct {
    tv_sec: isize,
    tv_nsec: isize,
};

pub const sigset_t = u32;
pub const empty_sigset = sigset_t(0);

/// Renamed from `sigaction` to `Sigaction` to avoid conflict with function name.
pub const Sigaction = extern struct {
    handler: extern fn (c_int) void,
    sa_mask: sigset_t,
    sa_flags: c_int,
};

pub const dirent = extern struct {
    d_ino: usize,
    d_seekoff: usize,
    d_reclen: u16,
    d_namlen: u16,
    d_type: u8,
    d_name: u8, // field address is address of first byte of name

    pub fn reclen(self: dirent) u16 {
        return self.d_reclen;
    }
};

pub const pthread_attr_t = extern struct {
    __sig: c_long,
    __opaque: [56]u8,
};

/// Renamed from `kevent` to `Kevent` to avoid conflict with function name.
pub const Kevent = extern struct {
    ident: usize,
    filter: i16,
    flags: u16,
    fflags: u32,
    data: isize,
    udata: usize,
};

// sys/types.h on macos uses #pragma pack(4) so these checks are
// to make sure the struct is laid out the same. These values were
// produced from C code using the offsetof macro.
comptime {
    assert(@byteOffsetOf(Kevent, "ident") == 0);
    assert(@byteOffsetOf(Kevent, "filter") == 8);
    assert(@byteOffsetOf(Kevent, "flags") == 10);
    assert(@byteOffsetOf(Kevent, "fflags") == 12);
    assert(@byteOffsetOf(Kevent, "data") == 16);
    assert(@byteOffsetOf(Kevent, "udata") == 24);
}

pub const kevent64_s = extern struct {
    ident: u64,
    filter: i16,
    flags: u16,
    fflags: u32,
    data: i64,
    udata: u64,
    ext: [2]u64,
};

// sys/types.h on macos uses #pragma pack() so these checks are
// to make sure the struct is laid out the same. These values were
// produced from C code using the offsetof macro.
comptime {
    assert(@byteOffsetOf(kevent64_s, "ident") == 0);
    assert(@byteOffsetOf(kevent64_s, "filter") == 8);
    assert(@byteOffsetOf(kevent64_s, "flags") == 10);
    assert(@byteOffsetOf(kevent64_s, "fflags") == 12);
    assert(@byteOffsetOf(kevent64_s, "data") == 16);
    assert(@byteOffsetOf(kevent64_s, "udata") == 24);
    assert(@byteOffsetOf(kevent64_s, "ext") == 32);
}

pub const mach_port_t = c_uint;
pub const clock_serv_t = mach_port_t;
pub const clock_res_t = c_int;
pub const mach_port_name_t = natural_t;
pub const natural_t = c_uint;
pub const mach_timespec_t = extern struct {
    tv_sec: c_uint,
    tv_nsec: clock_res_t,
};
pub const kern_return_t = c_int;
pub const host_t = mach_port_t;
pub const CALENDAR_CLOCK = 1;

pub const PATH_MAX = 1024;

pub const STDIN_FILENO = 0;
pub const STDOUT_FILENO = 1;
pub const STDERR_FILENO = 2;

/// [MC2] no permissions
pub const PROT_NONE = 0x00;

/// [MC2] pages can be read
pub const PROT_READ = 0x01;

/// [MC2] pages can be written
pub const PROT_WRITE = 0x02;

/// [MC2] pages can be executed
pub const PROT_EXEC = 0x04;

/// allocated from memory, swap space
pub const MAP_ANONYMOUS = 0x1000;

/// map from file (default)
pub const MAP_FILE = 0x0000;

/// interpret addr exactly
pub const MAP_FIXED = 0x0010;

/// region may contain semaphores
pub const MAP_HASSEMAPHORE = 0x0200;

/// changes are private
pub const MAP_PRIVATE = 0x0002;

/// share changes
pub const MAP_SHARED = 0x0001;

/// don't cache pages for this mapping
pub const MAP_NOCACHE = 0x0400;

/// don't reserve needed swap area
pub const MAP_NORESERVE = 0x0040;
pub const MAP_FAILED = @intToPtr(*c_void, maxInt(usize));

/// [XSI] no hang in wait/no child to reap
pub const WNOHANG = 0x00000001;

/// [XSI] notify on stop, untraced child
pub const WUNTRACED = 0x00000002;

/// take signal on signal stack
pub const SA_ONSTACK = 0x0001;

/// restart system on signal return
pub const SA_RESTART = 0x0002;

/// reset to SIG_DFL when taking signal
pub const SA_RESETHAND = 0x0004;

/// do not generate SIGCHLD on child stop
pub const SA_NOCLDSTOP = 0x0008;

/// don't mask the signal we're delivering
pub const SA_NODEFER = 0x0010;

/// don't keep zombies around
pub const SA_NOCLDWAIT = 0x0020;

/// signal handler with SA_SIGINFO args
pub const SA_SIGINFO = 0x0040;

/// do not bounce off kernel's sigtramp
pub const SA_USERTRAMP = 0x0100;

/// signal handler with SA_SIGINFO args with 64bit   regs information
pub const SA_64REGSET = 0x0200;

pub const O_LARGEFILE = 0x0000;
pub const O_PATH = 0x0000;

pub const F_OK = 0;
pub const X_OK = 1;
pub const W_OK = 2;
pub const R_OK = 4;

/// open for reading only
pub const O_RDONLY = 0x0000;

/// open for writing only
pub const O_WRONLY = 0x0001;

/// open for reading and writing
pub const O_RDWR = 0x0002;

/// do not block on open or for data to become available
pub const O_NONBLOCK = 0x0004;

/// append on each write
pub const O_APPEND = 0x0008;

/// create file if it does not exist
pub const O_CREAT = 0x0200;

/// truncate size to 0
pub const O_TRUNC = 0x0400;

/// error if O_CREAT and the file exists
pub const O_EXCL = 0x0800;

/// atomically obtain a shared lock
pub const O_SHLOCK = 0x0010;

/// atomically obtain an exclusive lock
pub const O_EXLOCK = 0x0020;

/// do not follow symlinks
pub const O_NOFOLLOW = 0x0100;

/// allow open of symlinks
pub const O_SYMLINK = 0x200000;

/// descriptor requested for event notifications only
pub const O_EVTONLY = 0x8000;

/// mark as close-on-exec
pub const O_CLOEXEC = 0x1000000;

pub const O_ACCMODE = 3;
pub const O_ALERT = 536870912;
pub const O_ASYNC = 64;
pub const O_DIRECTORY = 1048576;
pub const O_DP_GETRAWENCRYPTED = 1;
pub const O_DP_GETRAWUNENCRYPTED = 2;
pub const O_DSYNC = 4194304;
pub const O_FSYNC = O_SYNC;
pub const O_NOCTTY = 131072;
pub const O_POPUP = 2147483648;
pub const O_SYNC = 128;

pub const SEEK_SET = 0x0;
pub const SEEK_CUR = 0x1;
pub const SEEK_END = 0x2;

pub const DT_UNKNOWN = 0;
pub const DT_FIFO = 1;
pub const DT_CHR = 2;
pub const DT_DIR = 4;
pub const DT_BLK = 6;
pub const DT_REG = 8;
pub const DT_LNK = 10;
pub const DT_SOCK = 12;
pub const DT_WHT = 14;

/// block specified signal set
pub const SIG_BLOCK = 1;

/// unblock specified signal set
pub const SIG_UNBLOCK = 2;

/// set specified signal set
pub const SIG_SETMASK = 3;

/// hangup
pub const SIGHUP = 1;

/// interrupt
pub const SIGINT = 2;

/// quit
pub const SIGQUIT = 3;

/// illegal instruction (not reset when caught)
pub const SIGILL = 4;

/// trace trap (not reset when caught)
pub const SIGTRAP = 5;

/// abort()
pub const SIGABRT = 6;

/// pollable event ([XSR] generated, not supported)
pub const SIGPOLL = 7;

/// compatibility
pub const SIGIOT = SIGABRT;

/// EMT instruction
pub const SIGEMT = 7;

/// floating point exception
pub const SIGFPE = 8;

/// kill (cannot be caught or ignored)
pub const SIGKILL = 9;

/// bus error
pub const SIGBUS = 10;

/// segmentation violation
pub const SIGSEGV = 11;

/// bad argument to system call
pub const SIGSYS = 12;

/// write on a pipe with no one to read it
pub const SIGPIPE = 13;

/// alarm clock
pub const SIGALRM = 14;

/// software termination signal from kill
pub const SIGTERM = 15;

/// urgent condition on IO channel
pub const SIGURG = 16;

/// sendable stop signal not from tty
pub const SIGSTOP = 17;

/// stop signal from tty
pub const SIGTSTP = 18;

/// continue a stopped process
pub const SIGCONT = 19;

/// to parent on child stop or exit
pub const SIGCHLD = 20;

/// to readers pgrp upon background tty read
pub const SIGTTIN = 21;

/// like TTIN for output if (tp->t_local&LTOSTOP)
pub const SIGTTOU = 22;

/// input/output possible signal
pub const SIGIO = 23;

/// exceeded CPU time limit
pub const SIGXCPU = 24;

/// exceeded file size limit
pub const SIGXFSZ = 25;

/// virtual time alarm
pub const SIGVTALRM = 26;

/// profiling time alarm
pub const SIGPROF = 27;

/// window size changes
pub const SIGWINCH = 28;

/// information request
pub const SIGINFO = 29;

/// user defined signal 1
pub const SIGUSR1 = 30;

/// user defined signal 2
pub const SIGUSR2 = 31;

/// no flag value
pub const KEVENT_FLAG_NONE = 0x000;

/// immediate timeout
pub const KEVENT_FLAG_IMMEDIATE = 0x001;

/// output events only include change
pub const KEVENT_FLAG_ERROR_EVENTS = 0x002;

/// add event to kq (implies enable)
pub const EV_ADD = 0x0001;

/// delete event from kq
pub const EV_DELETE = 0x0002;

/// enable event
pub const EV_ENABLE = 0x0004;

/// disable event (not reported)
pub const EV_DISABLE = 0x0008;

/// only report one occurrence
pub const EV_ONESHOT = 0x0010;

/// clear event state after reporting
pub const EV_CLEAR = 0x0020;

/// force immediate event output
/// ... with or without EV_ERROR
/// ... use KEVENT_FLAG_ERROR_EVENTS
///     on syscalls supporting flags
pub const EV_RECEIPT = 0x0040;

/// disable event after reporting
pub const EV_DISPATCH = 0x0080;

/// unique kevent per udata value
pub const EV_UDATA_SPECIFIC = 0x0100;

/// ... in combination with EV_DELETE
/// will defer delete until udata-specific
/// event enabled. EINPROGRESS will be
/// returned to indicate the deferral
pub const EV_DISPATCH2 = EV_DISPATCH | EV_UDATA_SPECIFIC;

/// report that source has vanished
/// ... only valid with EV_DISPATCH2
pub const EV_VANISHED = 0x0200;

/// reserved by system
pub const EV_SYSFLAGS = 0xF000;

/// filter-specific flag
pub const EV_FLAG0 = 0x1000;

/// filter-specific flag
pub const EV_FLAG1 = 0x2000;

/// EOF detected
pub const EV_EOF = 0x8000;

/// error, data contains errno
pub const EV_ERROR = 0x4000;

pub const EV_POLL = EV_FLAG0;
pub const EV_OOBAND = EV_FLAG1;

pub const EVFILT_READ = -1;
pub const EVFILT_WRITE = -2;

/// attached to aio requests
pub const EVFILT_AIO = -3;

/// attached to vnodes
pub const EVFILT_VNODE = -4;

/// attached to struct proc
pub const EVFILT_PROC = -5;

/// attached to struct proc
pub const EVFILT_SIGNAL = -6;

/// timers
pub const EVFILT_TIMER = -7;

/// Mach portsets
pub const EVFILT_MACHPORT = -8;

/// Filesystem events
pub const EVFILT_FS = -9;

/// User events
pub const EVFILT_USER = -10;

/// Virtual memory events
pub const EVFILT_VM = -12;

/// Exception events
pub const EVFILT_EXCEPT = -15;

pub const EVFILT_SYSCOUNT = 17;

/// On input, NOTE_TRIGGER causes the event to be triggered for output.
pub const NOTE_TRIGGER = 0x01000000;

/// ignore input fflags
pub const NOTE_FFNOP = 0x00000000;

/// and fflags
pub const NOTE_FFAND = 0x40000000;

/// or fflags
pub const NOTE_FFOR = 0x80000000;

/// copy fflags
pub const NOTE_FFCOPY = 0xc0000000;

/// mask for operations
pub const NOTE_FFCTRLMASK = 0xc0000000;
pub const NOTE_FFLAGSMASK = 0x00ffffff;

/// low water mark
pub const NOTE_LOWAT = 0x00000001;

/// OOB data
pub const NOTE_OOB = 0x00000002;

/// vnode was removed
pub const NOTE_DELETE = 0x00000001;

/// data contents changed
pub const NOTE_WRITE = 0x00000002;

/// size increased
pub const NOTE_EXTEND = 0x00000004;

/// attributes changed
pub const NOTE_ATTRIB = 0x00000008;

/// link count changed
pub const NOTE_LINK = 0x00000010;

/// vnode was renamed
pub const NOTE_RENAME = 0x00000020;

/// vnode access was revoked
pub const NOTE_REVOKE = 0x00000040;

/// No specific vnode event: to test for EVFILT_READ      activation
pub const NOTE_NONE = 0x00000080;

/// vnode was unlocked by flock(2)
pub const NOTE_FUNLOCK = 0x00000100;

/// process exited
pub const NOTE_EXIT = 0x80000000;

/// process forked
pub const NOTE_FORK = 0x40000000;

/// process exec'd
pub const NOTE_EXEC = 0x20000000;

/// shared with EVFILT_SIGNAL
pub const NOTE_SIGNAL = 0x08000000;

/// exit status to be returned, valid for child       process only
pub const NOTE_EXITSTATUS = 0x04000000;

/// provide details on reasons for exit
pub const NOTE_EXIT_DETAIL = 0x02000000;

/// mask for signal & exit status
pub const NOTE_PDATAMASK = 0x000fffff;
pub const NOTE_PCTRLMASK = (~NOTE_PDATAMASK);

pub const NOTE_EXIT_DETAIL_MASK = 0x00070000;
pub const NOTE_EXIT_DECRYPTFAIL = 0x00010000;
pub const NOTE_EXIT_MEMORY = 0x00020000;
pub const NOTE_EXIT_CSERROR = 0x00040000;

/// will react on memory          pressure
pub const NOTE_VM_PRESSURE = 0x80000000;

/// will quit on memory       pressure, possibly after cleaning up dirty state
pub const NOTE_VM_PRESSURE_TERMINATE = 0x40000000;

/// will quit immediately on      memory pressure
pub const NOTE_VM_PRESSURE_SUDDEN_TERMINATE = 0x20000000;

/// there was an error
pub const NOTE_VM_ERROR = 0x10000000;

/// data is seconds
pub const NOTE_SECONDS = 0x00000001;

/// data is microseconds
pub const NOTE_USECONDS = 0x00000002;

/// data is nanoseconds
pub const NOTE_NSECONDS = 0x00000004;

/// absolute timeout
pub const NOTE_ABSOLUTE = 0x00000008;

/// ext[1] holds leeway for power aware timers
pub const NOTE_LEEWAY = 0x00000010;

/// system does minimal timer coalescing
pub const NOTE_CRITICAL = 0x00000020;

/// system does maximum timer coalescing
pub const NOTE_BACKGROUND = 0x00000040;
pub const NOTE_MACH_CONTINUOUS_TIME = 0x00000080;

/// data is mach absolute time units
pub const NOTE_MACHTIME = 0x00000100;

pub const AF_UNSPEC = 0;
pub const AF_LOCAL = 1;
pub const AF_UNIX = AF_LOCAL;
pub const AF_INET = 2;
pub const AF_SYS_CONTROL = 2;
pub const AF_IMPLINK = 3;
pub const AF_PUP = 4;
pub const AF_CHAOS = 5;
pub const AF_NS = 6;
pub const AF_ISO = 7;
pub const AF_OSI = AF_ISO;
pub const AF_ECMA = 8;
pub const AF_DATAKIT = 9;
pub const AF_CCITT = 10;
pub const AF_SNA = 11;
pub const AF_DECnet = 12;
pub const AF_DLI = 13;
pub const AF_LAT = 14;
pub const AF_HYLINK = 15;
pub const AF_APPLETALK = 16;
pub const AF_ROUTE = 17;
pub const AF_LINK = 18;
pub const AF_XTP = 19;
pub const AF_COIP = 20;
pub const AF_CNT = 21;
pub const AF_RTIP = 22;
pub const AF_IPX = 23;
pub const AF_SIP = 24;
pub const AF_PIP = 25;
pub const AF_ISDN = 28;
pub const AF_E164 = AF_ISDN;
pub const AF_KEY = 29;
pub const AF_INET6 = 30;
pub const AF_NATM = 31;
pub const AF_SYSTEM = 32;
pub const AF_NETBIOS = 33;
pub const AF_PPP = 34;
pub const AF_MAX = 40;

pub const PF_UNSPEC = AF_UNSPEC;
pub const PF_LOCAL = AF_LOCAL;
pub const PF_UNIX = PF_LOCAL;
pub const PF_INET = AF_INET;
pub const PF_IMPLINK = AF_IMPLINK;
pub const PF_PUP = AF_PUP;
pub const PF_CHAOS = AF_CHAOS;
pub const PF_NS = AF_NS;
pub const PF_ISO = AF_ISO;
pub const PF_OSI = AF_ISO;
pub const PF_ECMA = AF_ECMA;
pub const PF_DATAKIT = AF_DATAKIT;
pub const PF_CCITT = AF_CCITT;
pub const PF_SNA = AF_SNA;
pub const PF_DECnet = AF_DECnet;
pub const PF_DLI = AF_DLI;
pub const PF_LAT = AF_LAT;
pub const PF_HYLINK = AF_HYLINK;
pub const PF_APPLETALK = AF_APPLETALK;
pub const PF_ROUTE = AF_ROUTE;
pub const PF_LINK = AF_LINK;
pub const PF_XTP = AF_XTP;
pub const PF_COIP = AF_COIP;
pub const PF_CNT = AF_CNT;
pub const PF_SIP = AF_SIP;
pub const PF_IPX = AF_IPX;
pub const PF_RTIP = AF_RTIP;
pub const PF_PIP = AF_PIP;
pub const PF_ISDN = AF_ISDN;
pub const PF_KEY = AF_KEY;
pub const PF_INET6 = AF_INET6;
pub const PF_NATM = AF_NATM;
pub const PF_SYSTEM = AF_SYSTEM;
pub const PF_NETBIOS = AF_NETBIOS;
pub const PF_PPP = AF_PPP;
pub const PF_MAX = AF_MAX;

pub const SYSPROTO_EVENT = 1;
pub const SYSPROTO_CONTROL = 2;

pub const SOCK_STREAM = 1;
pub const SOCK_DGRAM = 2;
pub const SOCK_RAW = 3;
pub const SOCK_RDM = 4;
pub const SOCK_SEQPACKET = 5;
pub const SOCK_MAXADDRLEN = 255;

pub const IPPROTO_ICMP = 1;
pub const IPPROTO_ICMPV6 = 58;
pub const IPPROTO_TCP = 6;
pub const IPPROTO_UDP = 17;
pub const IPPROTO_IP = 0;
pub const IPPROTO_IPV6 = 41;

fn wstatus(x: u32) u32 {
    return x & 0o177;
}
const wstopped = 0o177;
pub fn WEXITSTATUS(x: u32) u32 {
    return x >> 8;
}
pub fn WTERMSIG(x: u32) u32 {
    return wstatus(x);
}
pub fn WSTOPSIG(x: u32) u32 {
    return x >> 8;
}
pub fn WIFEXITED(x: u32) bool {
    return wstatus(x) == 0;
}
pub fn WIFSTOPPED(x: u32) bool {
    return wstatus(x) == wstopped and WSTOPSIG(x) != 0x13;
}
pub fn WIFSIGNALED(x: u32) bool {
    return wstatus(x) != wstopped and wstatus(x) != 0;
}

/// Operation not permitted
pub const EPERM = 1;

/// No such file or directory
pub const ENOENT = 2;

/// No such process
pub const ESRCH = 3;

/// Interrupted system call
pub const EINTR = 4;

/// Input/output error
pub const EIO = 5;

/// Device not configured
pub const ENXIO = 6;

/// Argument list too long
pub const E2BIG = 7;

/// Exec format error
pub const ENOEXEC = 8;

/// Bad file descriptor
pub const EBADF = 9;

/// No child processes
pub const ECHILD = 10;

/// Resource deadlock avoided
pub const EDEADLK = 11;

/// Cannot allocate memory
pub const ENOMEM = 12;

/// Permission denied
pub const EACCES = 13;

/// Bad address
pub const EFAULT = 14;

/// Block device required
pub const ENOTBLK = 15;

/// Device / Resource busy
pub const EBUSY = 16;

/// File exists
pub const EEXIST = 17;

/// Cross-device link
pub const EXDEV = 18;

/// Operation not supported by device
pub const ENODEV = 19;

/// Not a directory
pub const ENOTDIR = 20;

/// Is a directory
pub const EISDIR = 21;

/// Invalid argument
pub const EINVAL = 22;

/// Too many open files in system
pub const ENFILE = 23;

/// Too many open files
pub const EMFILE = 24;

/// Inappropriate ioctl for device
pub const ENOTTY = 25;

/// Text file busy
pub const ETXTBSY = 26;

/// File too large
pub const EFBIG = 27;

/// No space left on device
pub const ENOSPC = 28;

/// Illegal seek
pub const ESPIPE = 29;

/// Read-only file system
pub const EROFS = 30;

/// Too many links
pub const EMLINK = 31;
/// Broken pipe

// math software
pub const EPIPE = 32;

/// Numerical argument out of domain
pub const EDOM = 33;
/// Result too large

// non-blocking and interrupt i/o
pub const ERANGE = 34;

/// Resource temporarily unavailable
pub const EAGAIN = 35;

/// Operation would block
pub const EWOULDBLOCK = EAGAIN;

/// Operation now in progress
pub const EINPROGRESS = 36;
/// Operation already in progress

// ipc/network software -- argument errors
pub const EALREADY = 37;

/// Socket operation on non-socket
pub const ENOTSOCK = 38;

/// Destination address required
pub const EDESTADDRREQ = 39;

/// Message too long
pub const EMSGSIZE = 40;

/// Protocol wrong type for socket
pub const EPROTOTYPE = 41;

/// Protocol not available
pub const ENOPROTOOPT = 42;

/// Protocol not supported
pub const EPROTONOSUPPORT = 43;

/// Socket type not supported
pub const ESOCKTNOSUPPORT = 44;

/// Operation not supported
pub const ENOTSUP = 45;

/// Protocol family not supported
pub const EPFNOSUPPORT = 46;

/// Address family not supported by protocol family
pub const EAFNOSUPPORT = 47;

/// Address already in use
pub const EADDRINUSE = 48;
/// Can't assign requested address

// ipc/network software -- operational errors
pub const EADDRNOTAVAIL = 49;

/// Network is down
pub const ENETDOWN = 50;

/// Network is unreachable
pub const ENETUNREACH = 51;

/// Network dropped connection on reset
pub const ENETRESET = 52;

/// Software caused connection abort
pub const ECONNABORTED = 53;

/// Connection reset by peer
pub const ECONNRESET = 54;

/// No buffer space available
pub const ENOBUFS = 55;

/// Socket is already connected
pub const EISCONN = 56;

/// Socket is not connected
pub const ENOTCONN = 57;

/// Can't send after socket shutdown
pub const ESHUTDOWN = 58;

/// Too many references: can't splice
pub const ETOOMANYREFS = 59;

/// Operation timed out
pub const ETIMEDOUT = 60;

/// Connection refused
pub const ECONNREFUSED = 61;

/// Too many levels of symbolic links
pub const ELOOP = 62;

/// File name too long
pub const ENAMETOOLONG = 63;

/// Host is down
pub const EHOSTDOWN = 64;

/// No route to host
pub const EHOSTUNREACH = 65;
/// Directory not empty

// quotas & mush
pub const ENOTEMPTY = 66;

/// Too many processes
pub const EPROCLIM = 67;

/// Too many users
pub const EUSERS = 68;
/// Disc quota exceeded

// Network File System
pub const EDQUOT = 69;

/// Stale NFS file handle
pub const ESTALE = 70;

/// Too many levels of remote in path
pub const EREMOTE = 71;

/// RPC struct is bad
pub const EBADRPC = 72;

/// RPC version wrong
pub const ERPCMISMATCH = 73;

/// RPC prog. not avail
pub const EPROGUNAVAIL = 74;

/// Program version wrong
pub const EPROGMISMATCH = 75;

/// Bad procedure for program
pub const EPROCUNAVAIL = 76;

/// No locks available
pub const ENOLCK = 77;

/// Function not implemented
pub const ENOSYS = 78;

/// Inappropriate file type or format
pub const EFTYPE = 79;

/// Authentication error
pub const EAUTH = 80;
/// Need authenticator

// Intelligent device errors
pub const ENEEDAUTH = 81;

/// Device power is off
pub const EPWROFF = 82;

/// Device error, e.g. paper out
pub const EDEVERR = 83;
/// Value too large to be stored in data type

// Program loading errors
pub const EOVERFLOW = 84;

/// Bad executable
pub const EBADEXEC = 85;

/// Bad CPU type in executable
pub const EBADARCH = 86;

/// Shared library version mismatch
pub const ESHLIBVERS = 87;

/// Malformed Macho file
pub const EBADMACHO = 88;

/// Operation canceled
pub const ECANCELED = 89;

/// Identifier removed
pub const EIDRM = 90;

/// No message of desired type
pub const ENOMSG = 91;

/// Illegal byte sequence
pub const EILSEQ = 92;

/// Attribute not found
pub const ENOATTR = 93;

/// Bad message
pub const EBADMSG = 94;

/// Reserved
pub const EMULTIHOP = 95;

/// No message available on STREAM
pub const ENODATA = 96;

/// Reserved
pub const ENOLINK = 97;

/// No STREAM resources
pub const ENOSR = 98;

/// Not a STREAM
pub const ENOSTR = 99;

/// Protocol error
pub const EPROTO = 100;

/// STREAM ioctl timeout
pub const ETIME = 101;

/// No such policy registered
pub const ENOPOLICY = 103;

/// State not recoverable
pub const ENOTRECOVERABLE = 104;

/// Previous owner died
pub const EOWNERDEAD = 105;

/// Interface output queue is full
pub const EQFULL = 106;

/// Must be equal largest errno
pub const ELAST = 106;

pub const SIGSTKSZ = 131072;
pub const MINSIGSTKSZ = 32768;

pub const SS_ONSTACK = 1;
pub const SS_DISABLE = 4;

pub const stack_t = extern struct {
    ss_sp: [*]u8,
    ss_size: isize,
    ss_flags: i32,
};

pub const S_IFMT = 0o170000;

pub const S_IFIFO = 0o010000;
pub const S_IFCHR = 0o020000;
pub const S_IFDIR = 0o040000;
pub const S_IFBLK = 0o060000;
pub const S_IFREG = 0o100000;
pub const S_IFLNK = 0o120000;
pub const S_IFSOCK = 0o140000;
pub const S_IFWHT = 0o160000;

pub const S_ISUID = 0o4000;
pub const S_ISGID = 0o2000;
pub const S_ISVTX = 0o1000;
pub const S_IRWXU = 0o700;
pub const S_IRUSR = 0o400;
pub const S_IWUSR = 0o200;
pub const S_IXUSR = 0o100;
pub const S_IRWXG = 0o070;
pub const S_IRGRP = 0o040;
pub const S_IWGRP = 0o020;
pub const S_IXGRP = 0o010;
pub const S_IRWXO = 0o007;
pub const S_IROTH = 0o004;
pub const S_IWOTH = 0o002;
pub const S_IXOTH = 0o001;

pub fn S_ISFIFO(m: u32) bool {
    return m & S_IFMT == S_IFIFO;
}

pub fn S_ISCHR(m: u32) bool {
    return m & S_IFMT == S_IFCHR;
}

pub fn S_ISDIR(m: u32) bool {
    return m & S_IFMT == S_IFDIR;
}

pub fn S_ISBLK(m: u32) bool {
    return m & S_IFMT == S_IFBLK;
}

pub fn S_ISREG(m: u32) bool {
    return m & S_IFMT == S_IFREG;
}

pub fn S_ISLNK(m: u32) bool {
    return m & S_IFMT == S_IFLNK;
}

pub fn S_ISSOCK(m: u32) bool {
    return m & S_IFMT == S_IFSOCK;
}

pub fn S_IWHT(m: u32) bool {
    return m & S_IFMT == S_IFWHT;
}
pub const HOST_NAME_MAX = 72;

pub const AT_FDCWD = -2;

/// Use effective ids in access check
pub const AT_EACCESS = 0x0010;

/// Act on the symlink itself not the target
pub const AT_SYMLINK_NOFOLLOW = 0x0020;

/// Act on target of symlink
pub const AT_SYMLINK_FOLLOW = 0x0040;

/// Path refers to directory
pub const AT_REMOVEDIR = 0x0080;

pub const addrinfo = extern struct {
    flags: i32,
    family: i32,
    socktype: i32,
    protocol: i32,
    addrlen: socklen_t,
    canonname: ?[*]u8,
    addr: ?*sockaddr,
    next: ?*addrinfo,
};
