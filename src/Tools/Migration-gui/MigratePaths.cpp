// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "MigratePaths.h"

#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <errno.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace MigrationGui {

namespace {

bool fileExists(const wxString &path)
{
  return wxFileName::FileExists(path);
}

wxString joinPath(const wxString &dir, const char *name)
{
  return wxFileName(dir, name).GetFullPath();
}

wxString normalizeDirPath(const wxString &input)
{
  wxString path = input;
  path.Trim(true).Trim(false);
  if (path.empty())
    return path;

  if (path[0] == '~')
  {
    const wxString home = wxGetHomeDir();
    if (path.length() == 1)
      path = home;
    else if (path[1] == '/' || path[1] == '\\')
      path = home + path.Mid(1);
  }

  // Treat input as a directory path (not a file). Otherwise GetPath() drops the
  // last component (e.g. /home/user/.conceal-mdbx becomes /home/user).
  wxFileName fn;
  fn.AssignDir(path);
  fn.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
  return fn.GetPath();
}

bool dirExists(const wxString &path)
{
  if (path.empty())
    return false;
  return wxDirExists(path);
}

#ifndef _WIN32
bool posixMkdirRecursive(const wxString &dirPath)
{
  const std::string p(dirPath.mb_str());
  if (p.empty())
    return false;

  struct stat st;
  if (stat(p.c_str(), &st) == 0)
    return S_ISDIR(st.st_mode);

  std::string accum;
  size_t pos = 0;
  while (pos <= p.size())
  {
    size_t next = p.find('/', pos);
    const size_t len = (next == std::string::npos) ? p.size() - pos : next - pos;
    const std::string part = p.substr(pos, len);

    if (!part.empty())
    {
      if (accum.empty())
        accum = part;
      else
        accum += '/' + part;

      if (stat(accum.c_str(), &st) != 0)
      {
        if (mkdir(accum.c_str(), 0755) != 0 && errno != EEXIST)
          return false;
      }
      else if (!S_ISDIR(st.st_mode))
      {
        return false;
      }
    }
    else if (next != std::string::npos && next == pos && accum.empty())
    {
      accum = "/";
    }

    if (next == std::string::npos)
      break;
    pos = next + 1;
  }

  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
#endif

bool mkdirRecursive(const wxString &dirPath)
{
  const wxString path = normalizeDirPath(dirPath);
  if (path.empty())
    return false;

  if (dirExists(path))
    return true;

#ifdef _WIN32
  wxFileName fn(path);
  return fn.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
#else
  if (posixMkdirRecursive(path))
    return true;
  wxFileName fn(path);
  return fn.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
#endif
}

} // namespace

wxString findMigrateBinary()
{
  wxString fromEnv;
  if (wxGetEnv("CONCEALD_MIGRATE", &fromEnv) && wxFileName::FileExists(fromEnv))
    return fromEnv;

  wxFileName dir(wxStandardPaths::Get().GetExecutablePath());
  dir.SetFullName(wxEmptyString);

  wxString previousPath;
  for (int depth = 0; depth < 6; ++depth)
  {
    const wxString dirPath = dir.GetPath();
    if (depth > 0 && dirPath == previousPath)
      break;
    previousPath = dirPath;

    wxString candidate = dir.GetPathWithSep() + wxString(MIGRATE_BINARY_NAME);
#ifdef __WXMSW__
    if (!wxFileName::FileExists(candidate))
      candidate += ".exe";
#endif
    if (wxFileName::FileExists(candidate))
      return candidate;

    dir.RemoveLastDir();
  }

  return wxEmptyString;
}

bool isValidOldBlockchainDir(const wxString &dir, wxString *detail)
{
  const wxString path = normalizeDirPath(dir);
  if (path.empty())
  {
    if (detail)
      *detail = "Select the old blockchain data directory.";
    return false;
  }

  if (!dirExists(path))
  {
    if (detail)
      *detail = "Directory does not exist.";
    return false;
  }

  if (!fileExists(joinPath(path, BLOCKS_FILE)))
  {
    if (detail)
      *detail = wxString::Format("Missing %s in selected directory.", BLOCKS_FILE);
    return false;
  }

  if (!fileExists(joinPath(path, BLOCKINDEXES_FILE)))
  {
    if (detail)
      *detail = wxString::Format("Missing %s in selected directory.", BLOCKINDEXES_FILE);
    return false;
  }

  if (detail)
    *detail = "blocks.dat and blockindexes.dat found.";
  return true;
}

bool migrationDirExists(const wxString &path)
{
  return dirExists(normalizeDirPath(path));
}

bool newDirHasExistingMdbx(const wxString &newDir, wxString *mdbxPath)
{
  const wxString base = normalizeDirPath(newDir);
  if (base.empty())
    return false;

  const wxString mdbx = joinPath(base, MDBX_BLOCKS_DIR);
  if (mdbxPath)
    *mdbxPath = mdbx;

  return wxDirExists(mdbx) || wxFileName::Exists(mdbx);
}

bool ensureNewDataDir(const wxString &newDir, wxString *detail, wxString *resolvedPath)
{
  const wxString path = normalizeDirPath(newDir);
  if (path.empty())
  {
    if (detail)
      *detail = "Enter a path for the new MDBX database directory.";
    return false;
  }

  if (resolvedPath)
    *resolvedPath = path;

  if (dirExists(path))
  {
    if (detail)
      *detail = wxString::Format("Directory already exists:\n%s", path);
    return true;
  }

  if (mkdirRecursive(path))
  {
    if (detail)
      *detail = wxString::Format("Created directory:\n%s", path);
    return true;
  }

  if (detail)
  {
    const char *err = strerror(errno);
    *detail = wxString::Format("mkdir failed for:\n%s\n(%s)", path, err ? err : "unknown error");
  }
  return false;
}

} // namespace MigrationGui
