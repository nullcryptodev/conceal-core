// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <wx/string.h>

namespace MigrationGui {

constexpr const char *BLOCKS_FILE = "blocks.dat";
constexpr const char *BLOCKINDEXES_FILE = "blockindexes.dat";
constexpr const char *MIGRATE_BINARY_NAME = "conceald-migrate";
constexpr const char *MDBX_BLOCKS_DIR = "mdbx_blocks";

/** Path to conceald-migrate: CONCEALD_MIGRATE, then search exe dir and parents (.., ../.., …). */
wxString findMigrateBinary();

/** True if dir contains legacy SwappedVector blockchain files. */
bool isValidOldBlockchainDir(const wxString &dir, wxString *detail = nullptr);

/** True if normalized path exists and is a directory (no create). */
bool migrationDirExists(const wxString &path);

/** True if <newDir>/mdbx_blocks already exists (migration would overwrite). */
bool newDirHasExistingMdbx(const wxString &newDir, wxString *mdbxPath = nullptr);

/** Ensures newDir exists (mkdir -p). On success, resolvedPath is the normalized absolute path. */
bool ensureNewDataDir(const wxString &newDir, wxString *detail = nullptr,
                      wxString *resolvedPath = nullptr);

} // namespace MigrationGui
