//! Minimal dependency-free PNG writer (RGBA8, filter 0, stored deflate).
//!
//! Phase B needs to dump `out/cube.png` from the render example without
//! pulling the `image` crate, so this implements just enough of the format:
//! 8-byte signature, IHDR (8-bit RGBA, no interlace), IDAT = zlib wrapper
//! around stored (uncompressed) deflate blocks, IEND. Every chunk carries a
//! standard CRC-32; the zlib stream ends with an Adler-32 checksum.

use std::fs;
use std::io;
use std::path::Path;

/// Write `rgba` (width*height*4 bytes, row-major, non-premultiplied) as PNG.
pub fn write_png(path: &Path, width: u32, height: u32, rgba: &[u8]) -> io::Result<()> {
    let expected = width as usize * height as usize * 4;
    if rgba.len() != expected {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("png: expected {expected} bytes, got {}", rgba.len()),
        ));
    }
    if width == 0 || width > u16::MAX as u32 {
        // Stored deflate blocks are built per scanline; a u16 bound keeps the
        // per-line block header math trivial.
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("png: unsupported width {width}"),
        ));
    }

    let mut out = Vec::with_capacity(expected / 2 + 1024);
    out.extend_from_slice(&[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]);

    // IHDR: width, height, bit depth 8, color type 6 (RGBA), compression 0,
    // filter 0 (adaptive value; we always emit filter type 0), interlace 0.
    let mut ihdr = Vec::with_capacity(13);
    ihdr.extend_from_slice(&width.to_be_bytes());
    ihdr.extend_from_slice(&height.to_be_bytes());
    ihdr.extend_from_slice(&[8, 6, 0, 0, 0]);
    push_chunk(&mut out, b"IHDR", &ihdr);

    // Raw image: each scanline prefixed with filter type 0 (None).
    let stride = width as usize * 4;
    let mut raw = Vec::with_capacity((stride + 1) * height as usize);
    for y in 0..height as usize {
        raw.push(0u8);
        raw.extend_from_slice(&rgba[y * stride..(y + 1) * stride]);
    }

    // zlib stream: header 0x78 0x01, stored deflate blocks (BTYPE=00),
    // Adler-32 of the uncompressed data.
    let mut idat = Vec::with_capacity(raw.len() + raw.len() / 65535 * 5 + 16);
    idat.extend_from_slice(&[0x78, 0x01]);
    let mut iter = raw.chunks(65535).peekable();
    while let Some(block) = iter.next() {
        let is_last = iter.peek().is_none();
        idat.push(u8::from(is_last)); // BFINAL | (BTYPE=00 << 1)
        let len = block.len() as u16;
        idat.extend_from_slice(&len.to_le_bytes());
        idat.extend_from_slice(&(!len).to_le_bytes());
        idat.extend_from_slice(block);
    }
    idat.extend_from_slice(&adler32(&raw).to_be_bytes());
    push_chunk(&mut out, b"IDAT", &idat);
    push_chunk(&mut out, b"IEND", &[]);

    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    fs::write(path, &out)
}

fn push_chunk(out: &mut Vec<u8>, tag: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(tag);
    out.extend_from_slice(data);
    let mut crc = CRC32_INIT;
    crc = crc32_update(crc, tag);
    crc = crc32_update(crc, data);
    out.extend_from_slice(&crc32_final(crc).to_be_bytes());
}

const CRC32_INIT: u32 = 0xFFFF_FFFF;

fn crc32_update(crc: u32, data: &[u8]) -> u32 {
    let mut crc = crc;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            let mask = (crc & 1).wrapping_neg();
            crc = (crc >> 1) ^ (0xEDB8_8320 & mask);
        }
    }
    crc
}

fn crc32_final(crc: u32) -> u32 {
    !crc
}

fn adler32(data: &[u8]) -> u32 {
    const MOD: u32 = 65521;
    let (mut a, mut b) = (1u32, 0u32);
    for &byte in data {
        a = (a + byte as u32) % MOD;
        b = (b + a) % MOD;
    }
    (b << 16) | a
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32_known_vector() {
        // CRC-32 of "123456789" is 0xCBF43926 (standard check value).
        let mut crc = CRC32_INIT;
        crc = crc32_update(crc, b"123456789");
        assert_eq!(crc32_final(crc), 0xCBF4_3926);
    }

    #[test]
    fn adler32_known_vector() {
        // Adler-32 of "Wikipedia" is 0x11E60398 (zlib spec example).
        assert_eq!(adler32(b"Wikipedia"), 0x11E6_0398);
    }

    #[test]
    fn writes_and_rereads_header() {
        let dir = std::env::temp_dir().join(format!("neb3-png-test-{}", std::process::id()));
        let path = dir.join("t.png");
        let px = [255u8, 0, 0, 255, 0, 255, 0, 128];
        write_png(&path, 2, 1, &px).unwrap();
        let bytes = fs::read(&path).unwrap();
        assert_eq!(&bytes[..8], &[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]);
        // IHDR chunk: length 13 + "IHDR"
        assert_eq!(&bytes[8..12], &13u32.to_be_bytes());
        assert_eq!(&bytes[12..16], b"IHDR");
        assert_eq!(&bytes[16..20], &2u32.to_be_bytes()); // width
        assert_eq!(&bytes[20..24], &1u32.to_be_bytes()); // height
        let _ = fs::remove_dir_all(&dir);
    }
}
