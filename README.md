# Blocked Data Storage Mechanism with Similarity-Aware Deduplication

A C++ storage system that detects duplicate and near-duplicate files at the chunk level, and stores similar content as a small "diff" instead of a full second copy.


---

## The Problem

Storage systems (cloud drives, backups, document stores) keep piling up duplicate and near-duplicate content — repeated uploads, slightly edited document versions, resized/recompressed images. Most systems only catch **exact** duplicates, so anything even slightly different gets stored as a brand-new, full copy. That wastes disk space and money as data grows.

## Why It Matters

- **Storage cost at scale** — deduplication is why Dropbox, Google Drive, Time Machine, and Borg invest heavily in this problem.
- **Version sprawl** — frequently re-saved files (papers, contracts, code) pile up near-identical copies; without delta storage, disk usage grows almost linearly with edits.
- **Media duplication** — the same photo often exists many times over (thumbnails, crops, recompressions), and exact-byte comparison misses all of them.
- **Proven approach** — this is the same core idea behind Git, rsync/Borg, and ZFS/Btrfs — real infrastructure we can benchmark against.

## What's New Here

1. **Cross-file, cross-type deduplication**
   Existing tools (Git, Dropbox) only catch duplicates *within* one file's history, or between files that are byte-identical. Our system chunks every file the same way regardless of type, so it can spot shared content across completely different formats (e.g. a paragraph that appears in both a `.docx` and a `.pdf`).

2. **Similarity-aware (fuzzy) matching**
   Instead of requiring byte-for-byte matches, we detect content that's just *similar*:
   - **Perceptual hashing** for images — catches crops, resizes, and recompression via Hamming distance.
   - **MinHash similarity** for text — catches overlapping content via estimated Jaccard similarity.

   Anything above a similarity threshold gets stored as a compact diff against its closest match, instead of as fresh data.

## Core Data Structures & Algorithms

| Component | Purpose |
|---|---|
| FastCdc | Content-defined chunking — boundaries survive local edits |
| xxhash3  (Hashing) | O(1) lookup for exact-duplicate chunks |
| Perceptual hashing + Hamming distance | Near-duplicate image detection |
| MinHash + LSH banding | Scalable text similarity search |
| Myers diff algorithm | Computes a compact delta once a similar chunk is found |
| Delta-chain graph + checkpointing | Bounds reconstruction cost on read |

## Why C++

- **Performance & memory control** — the system compares huge numbers of file chunks against each other, so speed matters.
- **OOP structure** — keeps chunking, hashing, diffing, and storage as separate, self-contained classes.
- **Fair comparison** — the tools we benchmark against (Git, rsync, ZFS) are also written in C/C++.

