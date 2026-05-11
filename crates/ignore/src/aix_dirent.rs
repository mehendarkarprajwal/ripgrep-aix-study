//! AIX-specific directory entry handling with d_type optimization
//!
//! This module provides AIX-specific optimizations for extracting file type
//! information from directory entries without requiring lstat() syscalls.
//!
//! ## Background
//!
//! On AIX with a custom kernel/libc patch, the `readdir()` function stores
//! file type information in the upper byte of the `d_namlen` field. This
//! avoids the expensive `lstat()` syscall that would otherwise be required
//! since AIX's JFS2 filesystem doesn't populate the `d_type` field.
//!
//! ## Layout of d_namlen
//!
//! ```text
//! d_namlen (2 bytes / ushort_t):
//! ┌──────────────────┬──────────────────┐
//! │   Byte 1 (8-15)  │   Byte 0 (0-7)   │
//! │   FILE TYPE      │   NAME LENGTH    │
//! │   (S_IFMT bits)  │   (0-255)        │
//! └──────────────────┴──────────────────┘
//! ```

use std::ffi::{CStr, OsStr, OsString};
use std::fs::FileType;
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::ptr;

#[cfg(target_os = "aix")]
use libc::{DIR, c_char, c_int, c_uint, c_ushort, ino64_t, mode_t, off64_t};

/// AIX dirent64 structure matching the system definition
///
/// This structure matches AIX's dirent64 with the custom patch that
/// stores file type in the upper byte of d_namlen.
#[cfg(target_os = "aix")]
#[repr(C)]
struct Dirent64 {
    d_offset: off64_t,     // 8 bytes: real offset after this entry
    d_ino: ino64_t,        // 8 bytes: inode number
    d_reclen: c_ushort,    // 2 bytes: length of this record
    d_namlen: c_ushort,    // 2 bytes: length of name + file type in upper byte
    d_name: [c_char; 256], // variable: name (null-terminated)
}

/// File type bit masks from AIX stat.h
#[cfg(target_os = "aix")]
mod file_type_bits {
    use libc::mode_t;

    pub const S_IFMT: mode_t = 0o170000; // File type mask
    pub const S_IFREG: mode_t = 0o100000; // Regular file
    pub const S_IFDIR: mode_t = 0o040000; // Directory
    pub const S_IFBLK: mode_t = 0o060000; // Block device
    pub const S_IFCHR: mode_t = 0o020000; // Character device
    pub const S_IFIFO: mode_t = 0o010000; // FIFO
    pub const S_IFLNK: mode_t = 0o120000; // Symbolic link
    pub const S_IFSOCK: mode_t = 0o140000; // Socket
}

/// Custom directory entry that includes pre-extracted file type
#[cfg(target_os = "aix")]
#[derive(Debug)]
pub struct AixDirEntry {
    /// Full path to the entry
    path: PathBuf,
    /// File name
    file_name: OsString,
    /// Inode number
    ino: u64,
    /// File type (if available from d_namlen)
    file_type: Option<FileType>,
}

#[cfg(target_os = "aix")]
impl AixDirEntry {
    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn file_name(&self) -> &OsStr {
        &self.file_name
    }

    pub fn ino(&self) -> u64 {
        self.ino
    }

    /// Get file type without calling lstat()
    ///
    /// Returns the file type that was extracted from d_namlen during readdir().
    /// If file type wasn't available, returns None (caller should use lstat()).
    pub fn file_type(&self) -> Option<FileType> {
        self.file_type
    }
}

/// Custom directory iterator for AIX
#[cfg(target_os = "aix")]
pub struct AixReadDir {
    dir_ptr: *mut DIR,
    dir_path: PathBuf,
}

#[cfg(target_os = "aix")]
impl AixReadDir {
    /// Open a directory for reading with AIX-specific optimizations
    pub fn open<P: AsRef<Path>>(path: P) -> io::Result<Self> {
        let path = path.as_ref();
        let c_path = std::ffi::CString::new(path.as_os_str().as_bytes())
            .map_err(|_| {
                io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "path contains null byte",
                )
            })?;

        let dir_ptr = unsafe { libc::opendir(c_path.as_ptr()) };

        if dir_ptr.is_null() {
            return Err(io::Error::last_os_error());
        }

        Ok(AixReadDir { dir_ptr, dir_path: path.to_path_buf() })
    }

    /// Read next directory entry
    pub fn read_entry(&mut self) -> io::Result<Option<AixDirEntry>> {
        unsafe {
            // Reset errno before calling readdir
            *libc::__errno() = 0;

            // Call readdir64 to get next entry
            let entry_ptr = libc::readdir64(self.dir_ptr) as *const Dirent64;

            if entry_ptr.is_null() {
                let errno = *libc::__errno();
                if errno == 0 {
                    // End of directory
                    return Ok(None);
                } else {
                    // Error occurred
                    return Err(io::Error::from_raw_os_error(errno));
                }
            }

            let entry = &*entry_ptr;

            // Extract file name
            let name_cstr = CStr::from_ptr(entry.d_name.as_ptr());
            let name_bytes = name_cstr.to_bytes();
            let file_name = OsStr::from_bytes(name_bytes).to_os_string();

            // Skip "." and ".."
            if name_bytes == b"." || name_bytes == b".." {
                return self.read_entry(); // Recursively get next entry
            }

            // Build full path
            let path = self.dir_path.join(&file_name);

            // Extract file type from d_namlen upper byte
            let file_type = extract_file_type_from_namlen(entry.d_namlen);

            Ok(Some(AixDirEntry {
                path,
                file_name,
                ino: entry.d_ino as u64,
                file_type,
            }))
        }
    }
}

#[cfg(target_os = "aix")]
impl Drop for AixReadDir {
    fn drop(&mut self) {
        unsafe {
            if !self.dir_ptr.is_null() {
                libc::closedir(self.dir_ptr);
            }
        }
    }
}

#[cfg(target_os = "aix")]
impl Iterator for AixReadDir {
    type Item = io::Result<AixDirEntry>;

    fn next(&mut self) -> Option<Self::Item> {
        match self.read_entry() {
            Ok(Some(entry)) => Some(Ok(entry)),
            Ok(None) => None,
            Err(e) => Some(Err(e)),
        }
    }
}

/// Extract file type from the upper byte of d_namlen
///
/// This function extracts the file type bits that were stored in the
/// upper byte of d_namlen by the patched AIX readdir() implementation.
///
/// # Arguments
///
/// * `d_namlen` - The 2-byte d_namlen value from dirent64
///
/// # Returns
///
/// * `Some(FileType)` - If file type bits are present and valid
/// * `None` - If no file type information is available (fallback to lstat)
#[cfg(target_os = "aix")]
pub fn extract_file_type_from_namlen(d_namlen: c_ushort) -> Option<FileType> {
    use file_type_bits::*;

    // Extract upper byte (file type bits)
    let type_byte = ((d_namlen >> 8) & 0xFF) as u8;

    // If upper byte is 0, no file type information is available
    if type_byte == 0 {
        return None;
    }

    // Convert to mode_t by shifting back to S_IFMT position
    // The upper byte contains bits 12-15 of st_mode, so shift left by 8
    let mode: mode_t = (type_byte as mode_t) << 8;

    // Create FileType from mode bits
    mode_to_file_type(mode)
}

/// Convert mode_t to FileType
///
/// This creates a Rust FileType from AIX mode bits.
#[cfg(target_os = "aix")]
fn mode_to_file_type(mode: mode_t) -> Option<FileType> {
    use file_type_bits::*;
    use std::os::unix::fs::FileTypeExt;

    // Create a dummy stat structure with just the mode set
    // SAFETY: We're creating a zeroed stat structure and only setting st_mode
    // This is safe because FileType only looks at the mode field
    unsafe {
        let mut stat: libc::stat = std::mem::zeroed();
        stat.st_mode = mode;

        // Convert stat to Metadata, then extract FileType
        // This uses the standard library's conversion logic
        let metadata = std::os::unix::fs::MetadataExt::from_raw_stat(stat);
        Some(metadata.file_type())
    }
}

/// Get file type string for debugging (matches test.c implementation)
#[cfg(target_os = "aix")]
#[allow(dead_code)]
pub fn get_file_type_string(mode: mode_t) -> &'static str {
    use file_type_bits::*;

    match mode & S_IFMT {
        S_IFREG => "Regular file",
        S_IFDIR => "Directory",
        S_IFLNK => "Symbolic link",
        S_IFCHR => "Character device",
        S_IFBLK => "Block device",
        S_IFIFO => "FIFO/pipe",
        S_IFSOCK => "Socket",
        _ => "Unknown",
    }
}

/// Get file type character (like ls -l)
#[cfg(target_os = "aix")]
#[allow(dead_code)]
pub fn get_file_type_char(mode: mode_t) -> char {
    use file_type_bits::*;

    match mode & S_IFMT {
        S_IFREG => '-',
        S_IFDIR => 'd',
        S_IFLNK => 'l',
        S_IFCHR => 'c',
        S_IFBLK => 'b',
        S_IFIFO => 'p',
        S_IFSOCK => 's',
        _ => '?',
    }
}

#[cfg(test)]
#[cfg(target_os = "aix")]
mod tests {
    use super::*;

    #[test]
    fn test_extract_regular_file() {
        // S_IFREG = 0100000 = 0x8000
        // Upper byte should be 0x20 (0100000 >> 12 = 0x20)
        let d_namlen: c_ushort = (0x20 << 8) | 10; // file type in upper byte, name length 10
        let file_type = extract_file_type_from_namlen(d_namlen);
        assert!(file_type.is_some());
        assert!(file_type.unwrap().is_file());
    }

    #[test]
    fn test_extract_directory() {
        // S_IFDIR = 0040000 = 0x4000
        // Upper byte should be 0x10
        let d_namlen: c_ushort = (0x10 << 8) | 5; // directory, name length 5
        let file_type = extract_file_type_from_namlen(d_namlen);
        assert!(file_type.is_some());
        assert!(file_type.unwrap().is_dir());
    }

    #[test]
    fn test_extract_symlink() {
        // S_IFLNK = 0120000 = 0xA000
        // Upper byte should be 0x28
        let d_namlen: c_ushort = (0x28 << 8) | 8; // symlink, name length 8
        let file_type = extract_file_type_from_namlen(d_namlen);
        assert!(file_type.is_some());
        assert!(file_type.unwrap().is_symlink());
    }

    #[test]
    fn test_no_file_type_info() {
        // Upper byte is 0 - no file type information
        let d_namlen: c_ushort = 0x0010; // name length 16, no type info
        let file_type = extract_file_type_from_namlen(d_namlen);
        assert!(file_type.is_none());
    }

    #[test]
    fn test_get_file_type_string() {
        use file_type_bits::*;

        assert_eq!(get_file_type_string(S_IFREG), "Regular file");
        assert_eq!(get_file_type_string(S_IFDIR), "Directory");
        assert_eq!(get_file_type_string(S_IFLNK), "Symbolic link");
        assert_eq!(get_file_type_string(S_IFCHR), "Character device");
        assert_eq!(get_file_type_string(S_IFBLK), "Block device");
        assert_eq!(get_file_type_string(S_IFIFO), "FIFO/pipe");
        assert_eq!(get_file_type_string(S_IFSOCK), "Socket");
    }

    #[test]
    fn test_get_file_type_char() {
        use file_type_bits::*;

        assert_eq!(get_file_type_char(S_IFREG), '-');
        assert_eq!(get_file_type_char(S_IFDIR), 'd');
        assert_eq!(get_file_type_char(S_IFLNK), 'l');
        assert_eq!(get_file_type_char(S_IFCHR), 'c');
        assert_eq!(get_file_type_char(S_IFBLK), 'b');
        assert_eq!(get_file_type_char(S_IFIFO), 'p');
        assert_eq!(get_file_type_char(S_IFSOCK), 's');
    }
}

// Made with Bob
