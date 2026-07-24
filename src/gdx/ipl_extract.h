// gdx-extract `ipl` subcommand (G-Diffuser R3 — IPL extraction).
//
// Slices the 64DD IPL/drive ROM's built-in font block into a small dedicated O2R archive
// (n64ddipl.o2r) so the port no longer needs N64DDIPLROM.n64 at runtime. See
// docs/investigation/2026-07-18/QUALITY_PARITY_ROADMAP.md §R3 (contracts C-R3.1..C-R3.5).
//
// The archive carries exactly two raw entries (no Torch resource header — these are plain blobs,
// read back verbatim by the port's O2rArchive::LoadFile):
//   ipl/font_block  = byte-order-NORMALIZED big-endian bytes [0xA0000, 0x140000) (0xA0000 bytes).
//   ipl/identity    = [u8 fmt][32-byte SHA-256 of the normalized full IPL].
//
// Output is written to <destDir>/n64ddipl.o2r using the same deterministic miniz path (MINIZ_NO_TIME
// + pinned deflate level + sorted-entry insertion) the o2r subcommand relies on, so two runs against
// the same IPL are byte-identical.
#pragma once

#include <string>

// Runs the IPL extraction. Returns 0 on success, non-zero on any error (bad path, too-small dump,
// archive write failure). On success writes <destDir>/n64ddipl.o2r and prints the normalized IPL
// SHA-256 + entry sizes to stdout (parsed by the runtime launcher for the completion sidecar).
int GdxRunIplExtract(const std::string& iplPath, const std::string& destDir);
