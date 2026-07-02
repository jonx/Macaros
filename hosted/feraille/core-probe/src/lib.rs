//! Feraille-on-AROS stage-1 probe: exercise real Feraille domain logic
//! (disk-usage treemap + HTML export, homoglyph name hazards) and return a
//! checksum the C harness asserts. Everything here is pure logic + std; the
//! point is proving the *crate graph* (uuid, hashbrown, etc.) builds and runs
//! for aarch64-unknown-aros, not the algorithms.

mod getrandom_aros;

use feraille_core::{name_hazards, NodeId};
use feraille_disk_usage::{
    build_layout_node, compute_treemap, treemap_html_document, DiskUsageTree, FileCategory,
    NodeKind, SizeMode,
};

fn fnv1a(data: &[u8]) -> u32 {
    let mut h: u32 = 0x811c9dc5;
    for &b in data {
        h ^= b as u32;
        h = h.wrapping_mul(0x01000193);
    }
    h
}

fn id(raw: u64) -> NodeId {
    NodeId::from_raw(raw).expect("nonzero")
}

/// Build a small tree, lay it out, export HTML, scan a deceptive filename.
/// Returns a digest over all results; 0 signals failure.
#[no_mangle]
pub extern "C" fn feraille_core_probe() -> u32 {
    let root = id(1);
    let mut tree = DiskUsageTree::new(root);
    tree.ensure_node_with_meta(root, NodeKind::Container, FileCategory::Other, None, "Work:", false);
    for (raw, name, cat, size) in [
        (2u64, "video.mov", FileCategory::Video, 700_000u64),
        (3, "notes.txt", FileCategory::Document, 50_000),
        (4, "song.mod", FileCategory::Audio, 250_000),
    ] {
        let n = id(raw);
        tree.ensure_node_with_meta(n, NodeKind::File, cat, None, name, false);
        tree.add_link(root, n);
        tree.add_size(n, size);
        tree.add_size(root, size);
    }

    let layout = build_layout_node(&tree, root, 3);
    let rects = compute_treemap(&layout, (0.0, 0.0, 640.0, 400.0), 3);
    if rects.len() != 4 {
        return 0;
    }
    // Areas must be size-proportional: the video rect dominates.
    let biggest_child = rects[1..]
        .iter()
        .max_by(|a, b| a.area().total_cmp(&b.area()))
        .expect("children");
    if biggest_child.size_bytes != 700_000 {
        return 0;
    }

    let html = treemap_html_document(&tree, root, SizeMode::Apparent, 640.0, 400.0, 3);
    if !html.contains("video.mov") || !html.starts_with("<!DOCTYPE html>") {
        return 0;
    }

    // Homoglyph detector: Cyrillic "а" in "pаypal.txt" must be flagged.
    let deceptive = "p\u{0430}ypal.txt";
    if !name_hazards::has_hazards(deceptive) || name_hazards::has_hazards("paypal.txt") {
        return 0;
    }
    let segs = name_hazards::analyze(deceptive);

    let mut acc = fnv1a(html.as_bytes());
    acc = acc.wrapping_add(rects.len() as u32 + segs.len() as u32);
    if acc == 0 {
        acc = 1;
    }
    acc
}

/// Stage-2 probe: bundled SQLite through feraille-meta — in-memory DB,
/// preference roundtrip, ant-trail visit recording. Returns 1 on success.
#[no_mangle]
pub extern "C" fn feraille_meta_probe() -> u32 {
    let db = match feraille_meta::MetadataDb::in_memory() {
        Ok(db) => db,
        Err(_) => return 0,
    };
    if db.set_preference("aros", "boing").is_err() {
        return 0;
    }
    if db.get_preference("aros").ok().flatten().as_deref() != Some("boing") {
        return 0;
    }
    if db.record_folder_visit("Work:Projects", 1_700_000_000).is_err() {
        return 0;
    }
    match db.load_recent_folders(5) {
        Ok(folders) if folders.iter().any(|f| f == "Work:Projects") => 1,
        _ => 0,
    }
}
