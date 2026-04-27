//! RSP packet parsing and formatting helpers.
//!
//! Covered subset (PR-2):
//!   g, G          — read/write all core registers
//!   m addr,len    — read memory
//!   M addr,len:xx — write memory
//!   s             — single step
//!   c             — continue
//!   Z1/z1         — hardware breakpoint set/clear
//!   Z2-Z4/z2-z4   — DWT watchpoint set/clear
//!
//! All other commands get an empty reply (""), which is the RSP "not
//! supported" response per the GDB remote serial protocol spec.

/// Parse the first complete RSP packet from `buf`.
///
/// Returns `(packet_data, bytes_consumed)`.  `bytes_consumed` includes the
/// `$`, the data, `#`, and the two checksum hex digits.  Leading `+`/`-`
/// acknowledgement bytes are skipped before scanning for `$`.
pub fn parse_rsp_packet(buf: &[u8]) -> Option<(String, usize)> {
    let start = buf.iter().position(|&b| b == b'$')?;
    let hash = buf[start + 1..].iter().position(|&b| b == b'#')
        .map(|p| start + 1 + p)?;
    if buf.len() < hash + 3 {
        return None; // checksum bytes not yet received
    }
    let data = std::str::from_utf8(&buf[start + 1..hash]).ok()?.to_string();
    Some((data, hash + 3))
}

/// Build `$<data>#<checksum>` from a plain string.
pub fn format_rsp_packet(data: &str) -> Vec<u8> {
    let ck: u8 = data.bytes().fold(0u8, |a, b| a.wrapping_add(b));
    format!("${}#{:02x}", data, ck).into_bytes()
}

/// Verify the two-character hex checksum `chk_hex` against `data`.
#[allow(dead_code)]
pub fn verify_checksum(data: &[u8], chk_hex: &[u8]) -> bool {
    if chk_hex.len() < 2 {
        return false;
    }
    let hi = match (chk_hex[0] as char).to_digit(16) {
        Some(d) => d as u8,
        None => return false,
    };
    let lo = match (chk_hex[1] as char).to_digit(16) {
        Some(d) => d as u8,
        None => return false,
    };
    let expected: u8 = data.iter().fold(0u8, |a, &b| a.wrapping_add(b));
    (hi << 4 | lo) == expected
}

/// Parse `addr,len` out of the argument portion of an `m` or `M` command.
pub fn parse_addr_len(args: &str) -> Option<(u64, usize)> {
    let mut parts = args.splitn(2, ',');
    let addr = u64::from_str_radix(parts.next()?.trim(), 16).ok()?;
    let len_str = parts.next()?.split(':').next()?; // M has `:data` after len
    let len = usize::from_str_radix(len_str.trim(), 16).ok()?;
    Some((addr, len))
}

/// Decode the hex-encoded byte string after `M addr,len:` into bytes.
pub fn decode_hex_bytes(hex: &str) -> Option<Vec<u8>> {
    if hex.len() % 2 != 0 {
        return None;
    }
    (0..hex.len() / 2)
        .map(|i| u8::from_str_radix(&hex[i * 2..i * 2 + 2], 16).ok())
        .collect()
}

/// Encode bytes as a lowercase hex string.
pub fn encode_hex_bytes(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{:02x}", b)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_basic_packet() {
        // $g#67
        let buf = b"$g#67";
        let (data, consumed) = parse_rsp_packet(buf).unwrap();
        assert_eq!(data, "g");
        assert_eq!(consumed, 5);
    }

    #[test]
    fn parse_skips_leading_ack() {
        let buf = b"+$g#67";
        let (data, consumed) = parse_rsp_packet(buf).unwrap();
        assert_eq!(data, "g");
        assert_eq!(consumed, 6);
    }

    #[test]
    fn parse_incomplete_returns_none() {
        assert!(parse_rsp_packet(b"$g#6").is_none()); // missing second checksum nibble
        assert!(parse_rsp_packet(b"$g").is_none());   // no hash yet
        assert!(parse_rsp_packet(b"").is_none());
    }

    #[test]
    fn parse_memory_read_packet() {
        // $m20000000,4#XX — checksum doesn't matter for parse_rsp_packet
        let data = "m20000000,4";
        let ck: u8 = data.bytes().fold(0, |a, b: u8| a.wrapping_add(b));
        let pkt = format!("${}#{:02x}", data, ck);
        let (out, _) = parse_rsp_packet(pkt.as_bytes()).unwrap();
        assert_eq!(out, data);
    }

    #[test]
    fn format_roundtrip() {
        let reply = "deadbeef";
        let pkt = format_rsp_packet(reply);
        let (parsed, _) = parse_rsp_packet(&pkt).unwrap();
        assert_eq!(parsed, reply);
    }

    #[test]
    fn format_empty_reply() {
        // Empty reply = RSP "not supported"
        let pkt = format_rsp_packet("");
        assert_eq!(pkt, b"$#00");
    }

    #[test]
    fn verify_checksum_ok() {
        let data = b"g";
        // sum('g') = 0x67
        assert!(verify_checksum(data, b"67"));
    }

    #[test]
    fn verify_checksum_bad() {
        assert!(!verify_checksum(b"g", b"00"));
    }

    #[test]
    fn parse_addr_len_basic() {
        let (addr, len) = parse_addr_len("20000000,4").unwrap();
        assert_eq!(addr, 0x20000000);
        assert_eq!(len, 4);
    }

    #[test]
    fn parse_addr_len_with_data_suffix() {
        // M command: "20000000,4:deadbeef"
        let (addr, len) = parse_addr_len("20000000,4:deadbeef").unwrap();
        assert_eq!(addr, 0x20000000);
        assert_eq!(len, 4);
    }

    #[test]
    fn decode_hex_bytes_basic() {
        assert_eq!(decode_hex_bytes("deadbeef").unwrap(), &[0xde, 0xad, 0xbe, 0xef]);
    }

    #[test]
    fn decode_hex_bytes_odd_returns_none() {
        assert!(decode_hex_bytes("abc").is_none());
    }

    #[test]
    fn encode_hex_bytes_basic() {
        assert_eq!(encode_hex_bytes(&[0xde, 0xad, 0xbe, 0xef]), "deadbeef");
    }
}
