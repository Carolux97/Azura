//! Describes the computer the launcher runs on, for status lines and the "Auto" graphics preset.

#[cfg(target_os = "macos")]
use std::process::Command;

use serde::Serialize;

/// Hardware summary sent to the interface as JSON.
#[derive(Debug, Default, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SystemInfo {
    /// Processor name, for example "Apple M4" or "AMD Ryzen 7".
    pub chip: Option<String>,
    /// Installed memory in whole gigabytes.
    pub memory_gb: Option<u64>,
    /// CPU architecture the launcher was built for ("x86_64" or "aarch64").
    pub arch: String,
    /// Operating system the launcher is running on ("linux" or "macos").
    pub os: String,
}

/// Reads one `sysctl` value on macOS, or `None` when the key does not exist.
#[cfg(target_os = "macos")]
fn sysctl(key: &str) -> Option<String> {
    let output = Command::new("/usr/sbin/sysctl").args(["-n", key]).output().ok()?;
    if !output.status.success() {
        return None;
    }
    let value = String::from_utf8(output.stdout).ok()?.trim().to_owned();
    (!value.is_empty()).then_some(value)
}

/// Converts a byte count to whole gibibytes (GiB).
fn bytes_to_gb(bytes: u64) -> u64 {
    bytes / (1024 * 1024 * 1024)
}

/// Reads the processor description from Linux's CPU information file.
#[cfg(target_os = "linux")]
fn linux_cpu_name() -> Option<String> {
    let contents = std::fs::read_to_string("/proc/cpuinfo").ok()?;
    contents.lines().find_map(|line| {
        let (key, value) = line.split_once(':')?;
        matches!(key.trim(), "model name" | "Hardware")
            .then(|| value.trim().to_owned())
            .filter(|value| !value.is_empty())
    })
}

/// Reads total memory from Linux's memory information file.
#[cfg(target_os = "linux")]
fn linux_memory_gb() -> Option<u64> {
    let contents = std::fs::read_to_string("/proc/meminfo").ok()?;
    parse_linux_memory_gb(&contents)
}

#[cfg(target_os = "linux")]
fn parse_linux_memory_gb(contents: &str) -> Option<u64> {
    let kilobytes = contents.lines().find_map(|line| {
        let (key, value) = line.split_once(':')?;
        (key == "MemTotal").then(|| value.split_whitespace().next()?.parse::<u64>().ok())?
    })?;
    Some(bytes_to_gb(kilobytes * 1024))
}

/// Collects the hardware summary.
pub fn system_info() -> SystemInfo {
    #[cfg(target_os = "macos")]
    let (chip, memory_gb) = (
        sysctl("machdep.cpu.brand_string"),
        sysctl("hw.memsize")
            .and_then(|bytes| bytes.parse().ok())
            .map(bytes_to_gb),
    );

    #[cfg(target_os = "linux")]
    let (chip, memory_gb) = (linux_cpu_name(), linux_memory_gb());

    #[cfg(not(any(target_os = "macos", target_os = "linux")))]
    let (chip, memory_gb) = (None, None);

    SystemInfo {
        chip,
        memory_gb,
        arch: std::env::consts::ARCH.to_owned(),
        os: std::env::consts::OS.to_owned(),
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
        #[cfg(target_os = "macos")]
        assert_eq!(sysctl("azura.no.such.key"), None);
    }

    #[test]
    fn reads_this_machine() {
        let info = system_info();
        assert!(!info.arch.is_empty());
        assert!(info.memory_gb.unwrap_or(0) > 0);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn parses_linux_memory_information() {
        let contents = "MemTotal:       16384000 kB\nMemFree:         1000000 kB\n";
        assert_eq!(parse_linux_memory_gb(contents), Some(15));
    }
}

