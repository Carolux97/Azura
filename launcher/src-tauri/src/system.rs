//! Describes the Mac the launcher runs on, for the status lines and the "Auto" graphics preset.
//!
//! The values come from `sysctl`, which every macOS install has. Each one is optional: a key that
//! a future macOS renames, or that an Intel Mac lacks, is reported as missing rather than failing
//! the whole call.

use std::process::Command;

use serde::Serialize;

/// Hardware summary sent to the interface as JSON.
#[derive(Debug, Default, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SystemInfo {
    /// Processor name, for example "Apple M4" or "Apple M3 Pro".
    pub chip: Option<String>,
    /// Installed memory in whole gigabytes.
    pub memory_gb: Option<u64>,
    /// CPU architecture the launcher was built for ("aarch64" on Apple Silicon).
    pub arch: String,
}

/// Reads one `sysctl` value as text, or `None` when the key does not exist.
fn sysctl(key: &str) -> Option<String> {
    let output = Command::new("/usr/sbin/sysctl").args(["-n", key]).output().ok()?;
    if !output.status.success() {
        return None;
    }
    let value = String::from_utf8(output.stdout).ok()?.trim().to_owned();
    (!value.is_empty()).then_some(value)
}

/// Converts a byte count to whole gigabytes (GiB, as macOS reports memory).
fn bytes_to_gb(bytes: u64) -> u64 {
    bytes / (1024 * 1024 * 1024)
}

/// Collects the hardware summary.
pub fn system_info() -> SystemInfo {
    SystemInfo {
        chip: sysctl("machdep.cpu.brand_string"),
        memory_gb: sysctl("hw.memsize")
            .and_then(|bytes| bytes.parse().ok())
            .map(bytes_to_gb),
        arch: std::env::consts::ARCH.to_owned(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn converts_memory_to_whole_gigabytes() {
        assert_eq!(bytes_to_gb(25_769_803_776), 24);
        assert_eq!(bytes_to_gb(8 * 1024 * 1024 * 1024 + 1), 8);
    }

    #[test]
    fn missing_keys_read_as_none() {
        assert_eq!(sysctl("azura.no.such.key"), None);
    }

    #[test]
    fn reads_this_machine() {
        let info = system_info();
        assert!(!info.arch.is_empty());
        // Every macOS reports its memory; the chip name is not checked because CI may vary.
        assert!(info.memory_gb.unwrap_or(0) > 0);
    }
}
