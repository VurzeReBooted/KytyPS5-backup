// Separate read-only process using the pinned pkg-to-anyps5 decoder modules.
mod bytes;
mod error;
mod fih;
mod inner;
mod kraken;
mod naps;
mod pfs;
mod source;

use std::collections::BTreeMap;
use std::io::{self, Read, Write};
use std::path::Path;
use error::{Error, Result};
use source::Source;

enum Member {
    Image { offset: u64, size: u64 },
    Metadata(Vec<u8>),
}

impl Member {
    fn size(&self) -> u64 {
        match self {
            Self::Image { size, .. } => *size,
            Self::Metadata(bytes) => bytes.len() as u64,
        }
    }
}

fn serve(path: &Path) -> Result<()> {
    let src = Source::open(path)?;
    let header = fih::Fih::read(&src)?;
    let outer = pfs::OuterPfs::open(&src, header.pfs_offset, header.pfs_size, header.superblock_offset)?;
    let open = |name: &str| outer.file(outer.find(name)?.ok_or_else(|| Error::format(format!("missing {name}")))?);
    let layout = open("naps_pkg_layout.dat")?;
    if layout.size > 1 << 30 { return Err(Error::format("oversize NAPS layout")); }
    let image = inner::InnerImage::open(open("pfs_image.dat")?, &layout.read_all()?)?;
    let mut index = BTreeMap::new();
    for file in fih::Cnt::read(&src, header.cnt_offset)?.files {
        index.insert(format!("sce_sys/{}", file.name), Member::Metadata(file.bytes));
    }
    for file in image.files()? {
        if !fih::is_plain_relative_path(&file.path) { return Err(Error::format("invalid member name")); }
        index.insert(file.path, Member::Image { offset: file.logical, size: file.size });
    }
    if !index.contains_key("eboot.bin") { return Err(Error::format("package contains no eboot.bin")); }

    // The index order supplies each file's ID. Integers on the wire are little endian.
    let pipe_error = |e| Error::Io("PKG reader pipe".into(), e);
    let mut output = io::BufWriter::new(io::stdout().lock());
    output.write_all(b"KPR1").map_err(pipe_error)?;
    output.write_all(&(index.len() as u32).to_le_bytes()).map_err(pipe_error)?;
    for (name, member) in &index {
        output.write_all(&(name.len() as u32).to_le_bytes()).map_err(pipe_error)?;
        output.write_all(&member.size().to_le_bytes()).map_err(pipe_error)?;
        output.write_all(name.as_bytes()).map_err(pipe_error)?;
    }
    output.flush().map_err(pipe_error)?;
    let files: Vec<_> = index.into_values().collect();
    let mut input = io::stdin().lock();
    let mut data = Vec::new();
    loop {
        let mut request = [0u8; 16];
        match input.read_exact(&mut request) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(e) => return Err(pipe_error(e)),
        }
        let id = u32::from_le_bytes(request[0..4].try_into().unwrap());
        let offset = u64::from_le_bytes(request[4..12].try_into().unwrap());
        let count = u32::from_le_bytes(request[12..16].try_into().unwrap());
        let file = files.get(id as usize).ok_or_else(|| Error::format("unknown file ID"))?;
        if count > 1 << 20 || offset.checked_add(count as u64).is_none_or(|end| end > file.size()) {
            return Err(Error::format("read outside member"));
        }
        match file {
            Member::Image { offset: base, .. } => {
                data.resize(count as usize, 0);
                image.read_at(base + offset, &mut data)?;
                output.write_all(&data).map_err(pipe_error)?;
            }
            Member::Metadata(bytes) => {
                output.write_all(&bytes[offset as usize..offset as usize + count as usize]).map_err(pipe_error)?;
            }
        }
        output.flush().map_err(pipe_error)?;
    }
}

fn main() {
    let Some(path) = std::env::args_os().nth(1) else { std::process::exit(1) };
    if let Err(e) = serve(Path::new(&path)) {
        eprintln!("Kyty PKG reader: {e}");
        std::process::exit(1);
    }
}
