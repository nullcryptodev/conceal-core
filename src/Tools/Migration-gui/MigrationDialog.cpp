// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "MigrationDialog.h"

#include "MigratePaths.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dir.h>
#include <wx/filepicker.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {

/** Conceal brand orange (#ffa500). */
const wxColour kConcealOrange(0xFF, 0xA5, 0x00);

long executeArgv(const wxArrayString &argv, int flags, wxProcess *process)
{
  const size_t argc = argv.size();
  char **cargv = new char *[argc + 1];
  for (size_t i = 0; i < argc; ++i)
    cargv[i] = wxStrdup(argv[i].mb_str(wxConvUTF8));
  cargv[argc] = nullptr;

  const long pid = wxExecute(cargv, flags, process);

  for (size_t i = 0; i < argc; ++i)
    free(cargv[i]);
  delete[] cargv;
  return pid;
}

void drainStream(wxInputStream *stream, wxTextCtrl *log)
{
  if (!stream || !stream->CanRead())
    return;

  char buffer[4096];
  while (stream->CanRead())
  {
    stream->Read(buffer, sizeof(buffer) - 1);
    size_t n = stream->LastRead();
    if (n == 0)
      break;
    buffer[n] = '\0';
    log->AppendText(wxString::FromUTF8(buffer));
  }
}

} // namespace

wxBEGIN_EVENT_TABLE(MigrationDialog, wxDialog)
  EVT_BUTTON(ID_CREATE_DIR, MigrationDialog::onCreateDir)
  EVT_BUTTON(ID_CANCEL, MigrationDialog::onCancel)
  EVT_BUTTON(ID_MIGRATE, MigrationDialog::onMigrate)
  EVT_DIRPICKER_CHANGED(ID_OLD_DIR, MigrationDialog::onOldDirChanged)
  EVT_DIRPICKER_CHANGED(ID_NEW_DIR, MigrationDialog::onNewDirChanged)
  EVT_TIMER(ID_POLL_TIMER, MigrationDialog::onPollTimer)
  EVT_END_PROCESS(wxID_ANY, MigrationDialog::onProcessTerminated)
wxEND_EVENT_TABLE()

MigrationDialog::MigrationDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, "Conceal MDBX Migration",
               wxDefaultPosition, wxSize(720, 520),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_oldDirPicker(nullptr),
      m_newDirPicker(nullptr),
      m_oldStatus(nullptr),
      m_newStatus(nullptr),
      m_testnetCheck(nullptr),
      m_skipValidationCheck(nullptr),
      m_batchSizeSpin(nullptr),
      m_log(nullptr),
      m_migrateBtn(nullptr),
      m_cancelBtn(nullptr),
      m_process(nullptr),
      m_pollTimer(this, ID_POLL_TIMER),
      m_childPid(0),
      m_running(false),
      m_stopRequested(false)
{
  buildLayout();

  const wxString migrateBin = MigrationGui::findMigrateBinary();
  if (migrateBin.empty())
  {
    appendLog("Warning: conceald-migrate not found (searched this folder and parent directories).\n"
              "Set CONCEALD_MIGRATE or build MigrationTool (usually in ../ or ../../).\n\n");
  }
  else
  {
    appendLog(wxString::Format("Using migration tool: %s\n\n", migrateBin));
  }

  updateOldStatus();
  updateNewStatus();
  Centre();
}

void MigrationDialog::buildLayout()
{
  auto *root = new wxBoxSizer(wxVERTICAL);

  root->Add(new wxStaticText(this, wxID_ANY,
                             "Migrate legacy SwappedVector blockchain data to MDBX."),
            0, wxALL | wxEXPAND, 10);

  auto *columns = new wxBoxSizer(wxHORIZONTAL);

  // ── Old directory (left) ─────────────────────────────────────────────
  auto *oldBoxSizer = new wxStaticBoxSizer(wxVERTICAL, this, "Old blockchain data");
  m_oldDirPicker = new wxDirPickerCtrl(this, ID_OLD_DIR, wxEmptyString,
                                       "Select directory with blocks.dat",
                                       wxDefaultPosition, wxDefaultSize,
                                       wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
  oldBoxSizer->Add(m_oldDirPicker, 0, wxEXPAND | wxALL, 6);
  m_oldStatus = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                 wxST_ELLIPSIZE_END);
  oldBoxSizer->Add(m_oldStatus, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
  columns->Add(oldBoxSizer, 1, wxEXPAND | wxRIGHT, 6);

  // ── New directory (right) — picker text field + browse at end; type or append path
  auto *newBoxSizer = new wxStaticBoxSizer(wxVERTICAL, this, "New MDBX database");
  m_newDirPicker = new wxDirPickerCtrl(this, ID_NEW_DIR, wxEmptyString,
                                       "Select or type new MDBX data directory",
                                       wxDefaultPosition, wxDefaultSize,
                                       wxDIRP_USE_TEXTCTRL);
  newBoxSizer->Add(m_newDirPicker, 0, wxEXPAND | wxALL, 6);

  newBoxSizer->Add(new wxButton(this, ID_CREATE_DIR, "Create folder"), 0,
                    wxLEFT | wxRIGHT | wxBOTTOM, 6);
  m_newStatus = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                 wxST_ELLIPSIZE_END);
  newBoxSizer->Add(m_newStatus, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

  columns->Add(newBoxSizer, 1, wxEXPAND | wxLEFT, 6);
  root->Add(columns, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

  // ── Options ──────────────────────────────────────────────────────────
  auto *optBox = new wxStaticBoxSizer(wxVERTICAL, this, "Options");
  m_testnetCheck = new wxCheckBox(this, ID_TESTNET, "Testnet");
  m_skipValidationCheck = new wxCheckBox(this, ID_SKIP_VALIDATION,
                                         "Skip chain validation (faster; daemon may reject bad data)");
  optBox->Add(m_testnetCheck, 0, wxALL, 6);
  optBox->Add(m_skipValidationCheck, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

  auto *batchRow = new wxBoxSizer(wxHORIZONTAL);
  batchRow->Add(new wxStaticText(this, wxID_ANY, "Batch size (blocks):"), 0,
                wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  m_batchSizeSpin = new wxSpinCtrl(this, ID_BATCH_SIZE, wxEmptyString,
                                   wxDefaultPosition, wxDefaultSize,
                                   wxSP_ARROW_KEYS, 1000, 100000, 50000);
  batchRow->Add(m_batchSizeSpin, 0, wxALIGN_CENTER_VERTICAL);
  optBox->Add(batchRow, 0, wxALL, 6);
  root->Add(optBox, 0, wxEXPAND | wxALL, 8);

  // ── Log ──────────────────────────────────────────────────────────────
  root->Add(new wxStaticText(this, wxID_ANY, "Output:"), 0, wxLEFT | wxRIGHT, 10);
  m_log = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                         wxDefaultPosition, wxSize(-1, 140),
                         wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
  root->Add(m_log, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  // ── Buttons ──────────────────────────────────────────────────────────
  auto *btnRow = new wxBoxSizer(wxHORIZONTAL);
  btnRow->AddStretchSpacer();
  m_cancelBtn = new wxButton(this, ID_CANCEL, "Cancel");
  m_migrateBtn = new wxButton(this, ID_MIGRATE, "Migrate");
  m_migrateBtn->SetDefault();
  btnRow->Add(m_cancelBtn, 0, wxRIGHT, 8);
  btnRow->Add(m_migrateBtn, 0);
  root->Add(btnRow, 0, wxEXPAND | wxALL, 10);

  SetSizer(root);

  m_oldDirPicker->GetTextCtrl()->Bind(wxEVT_TEXT, &MigrationDialog::onOldDirText, this);
  m_newDirPicker->GetTextCtrl()->Bind(wxEVT_TEXT, &MigrationDialog::onNewDirText, this);
}

wxString MigrationDialog::oldDirPath() const
{
  wxString path = m_oldDirPicker->GetTextCtrlValue();
  if (path.empty())
    path = m_oldDirPicker->GetPath();
  return path.Trim().Trim(false);
}

wxString MigrationDialog::newDirPath() const
{
  if (!m_newDirPicker)
    return wxEmptyString;
  wxString path = m_newDirPicker->GetTextCtrlValue();
  if (path.empty())
    path = m_newDirPicker->GetPath();
  return path.Trim().Trim(false);
}

void MigrationDialog::updateOldStatus()
{
  wxString detail;
  const bool ok = MigrationGui::isValidOldBlockchainDir(oldDirPath(), &detail);
  m_oldStatus->SetLabel(detail);
  m_oldStatus->SetForegroundColour(ok ? kConcealOrange : *wxRED);
}

void MigrationDialog::updateNewStatus()
{
  const wxString path = newDirPath();
  if (path.empty())
  {
    m_newStatus->UnsetToolTip();
    m_newStatus->SetLabel("Enter path, browse, or create folder.");
    m_newStatus->SetForegroundColour(*wxBLACK);
    return;
  }

  if (!MigrationGui::migrationDirExists(path))
  {
    m_newStatus->UnsetToolTip();
    m_newStatus->SetLabel("Path not found.");
    m_newStatus->SetForegroundColour(wxColour(120, 80, 0));
    return;
  }

  wxString mdbxPath;
  if (MigrationGui::newDirHasExistingMdbx(path, &mdbxPath))
  {
    m_newStatus->SetLabel("mdbx_blocks present — will overwrite.");
    m_newStatus->SetForegroundColour(*wxRED);
    m_newStatus->SetToolTip(
        wxString::Format("MDBX data already exists:\n%s\n\n"
                         "It will be overwritten if you continue.",
                         mdbxPath));
    return;
  }

  m_newStatus->UnsetToolTip();
  m_newStatus->SetLabel("Ready for migration.");
  m_newStatus->SetForegroundColour(*wxBLACK);
}

void MigrationDialog::onOldDirChanged(wxFileDirPickerEvent &)
{
  updateOldStatus();
}

void MigrationDialog::onOldDirText(wxCommandEvent &)
{
  updateOldStatus();
}

void MigrationDialog::onNewDirChanged(wxFileDirPickerEvent &)
{
  updateNewStatus();
}

void MigrationDialog::onNewDirText(wxCommandEvent &)
{
  updateNewStatus();
}

void MigrationDialog::onCreateDir(wxCommandEvent &)
{
  const wxString raw = newDirPath();
  if (raw.empty())
  {
    wxMessageBox("Type a directory path in the New MDBX database field first.",
                 "Create folder", wxOK | wxICON_WARNING, this);
    m_newDirPicker->GetTextCtrl()->SetFocus();
    return;
  }

  wxString detail;
  wxString resolved;
  if (!MigrationGui::ensureNewDataDir(raw, &detail, &resolved))
  {
    wxMessageBox(detail, "Create folder", wxOK | wxICON_WARNING, this);
    return;
  }

  m_newDirPicker->SetPath(resolved);
  updateNewStatus();

  if (MigrationGui::newDirHasExistingMdbx(resolved))
  {
    wxMessageBox("This folder already contains mdbx_blocks.\n\n"
                 "/!\\ It will be overwritten if you run Migrate.",
                 "Create folder", wxOK | wxICON_WARNING, this);
  }
  else
    wxMessageBox(detail, "Create folder", wxOK | wxICON_INFORMATION, this);
}

bool MigrationDialog::validateInputs(wxString *error) const
{
  const wxString oldDir = oldDirPath();
  const wxString newDir = newDirPath();

  wxString detail;
  if (!MigrationGui::isValidOldBlockchainDir(oldDir, &detail))
  {
    if (error)
      *error = detail;
    return false;
  }

  if (oldDir == newDir)
  {
    if (error)
      *error = "Old and new directories must be different.";
    return false;
  }

  if (newDir.empty())
  {
    if (error)
      *error = "Enter the new MDBX database directory path.";
    return false;
  }

  if (MigrationGui::findMigrateBinary().empty())
  {
    if (error)
      *error = "conceald-migrate not found. Build MigrationTool or set CONCEALD_MIGRATE.";
    return false;
  }

  return true;
}

wxArrayString MigrationDialog::buildMigrateArgv() const
{
  wxArrayString argv;
  argv.Add(MigrationGui::findMigrateBinary());
  argv.Add("--old-dir");
  argv.Add(oldDirPath());
  argv.Add("--new-dir");
  argv.Add(newDirPath());
  if (m_testnetCheck->GetValue())
    argv.Add("--testnet");
  if (m_skipValidationCheck->GetValue())
    argv.Add("--skip-validation");
  argv.Add("--batch-size");
  argv.Add(wxString::Format("%d", m_batchSizeSpin->GetValue()));
  return argv;
}

void MigrationDialog::stopMigrationProcess()
{
  if (m_childPid <= 0)
    return;

#ifdef _WIN32
  wxKillError err = wxKILL_NO_PROCESS;
  if (wxKill(m_childPid, wxSIGTERM, &err, wxKILL_CHILDREN) != 0 ||
      err == wxKILL_NO_PROCESS)
    wxKill(m_childPid, wxSIGKILL, NULL, wxKILL_CHILDREN);
#else
  // wxEXEC_MAKE_GROUP_LEADER: negative pid targets the whole group
  if (kill(-m_childPid, SIGTERM) != 0)
    kill(m_childPid, SIGTERM);
  // Ensure stop is immediate (migration may ignore SIGTERM during heavy I/O)
  if (kill(-m_childPid, SIGKILL) != 0)
    kill(m_childPid, SIGKILL);
  wxKill(m_childPid, wxSIGKILL, NULL, wxKILL_CHILDREN);
#endif
}

void MigrationDialog::setFormEnabled(bool enabled)
{
  m_oldDirPicker->Enable(enabled);
  m_newDirPicker->Enable(enabled);
  m_testnetCheck->Enable(enabled);
  m_skipValidationCheck->Enable(enabled);
  m_batchSizeSpin->Enable(enabled);
  if (wxWindow *createBtn = FindWindow(ID_CREATE_DIR))
    createBtn->Enable(enabled);
  m_migrateBtn->Enable(enabled);
}

void MigrationDialog::appendLog(const wxString &text)
{
  m_log->AppendText(text);
}

void MigrationDialog::onMigrate(wxCommandEvent &)
{
  if (m_running)
    return;

  wxString error;
  if (!validateInputs(&error))
  {
    wxMessageBox(error, "Validation", wxOK | wxICON_WARNING, this);
    return;
  }

  wxString mkdirDetail;
  wxString resolvedNewDir;
  if (!MigrationGui::ensureNewDataDir(newDirPath(), &mkdirDetail, &resolvedNewDir))
  {
    wxMessageBox(mkdirDetail, "New directory", wxOK | wxICON_WARNING, this);
    return;
  }
  m_newDirPicker->SetPath(resolvedNewDir);
  updateNewStatus();

  wxString mdbxPath;
  if (MigrationGui::newDirHasExistingMdbx(resolvedNewDir, &mdbxPath))
  {
    const int answer = wxMessageBox(
        wxString::Format(
            "MDBX data already exists in the new folder:\n%s\n\n"
            "/!\\ It will be overwritten if you continue.",
            mdbxPath),
        "Overwrite MDBX data?", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    if (answer != wxYES)
      return;
  }

  const wxArrayString argv = buildMigrateArgv();
  appendLog("\n--- Starting migration ---\n");
  for (size_t i = 0; i < argv.size(); ++i)
  {
    if (i > 0)
      appendLog(" ");
    appendLog(argv[i]);
  }
  appendLog("\n\n");

  m_process = new wxProcess(this);
  m_process->Redirect();

  m_stopRequested = false;
  const int execFlags = wxEXEC_ASYNC | wxEXEC_MAKE_GROUP_LEADER;
  m_childPid = executeArgv(argv, execFlags, m_process);
  if (m_childPid == 0)
  {
    appendLog("Failed to start conceald-migrate.\n");
    delete m_process;
    m_process = nullptr;
    return;
  }

  m_running = true;
  setFormEnabled(false);
  m_cancelBtn->SetLabel("Stop");
  m_pollTimer.Start(150);
}

void MigrationDialog::onCancel(wxCommandEvent &)
{
  if (m_running && m_childPid != 0)
  {
    m_stopRequested = true;
    m_cancelBtn->Enable(false);
    appendLog("\n--- Stopping migration (SIGKILL) ---\n");
    stopMigrationProcess();
    return;
  }

  EndModal(wxID_CANCEL);
}

void MigrationDialog::onPollTimer(wxTimerEvent &)
{
  if (!m_process)
    return;

  drainStream(m_process->GetInputStream(), m_log);
  drainStream(m_process->GetErrorStream(), m_log);
}

void MigrationDialog::onProcessTerminated(wxProcessEvent &event)
{
  if (event.GetPid() != m_childPid)
    return;

  m_pollTimer.Stop();
  drainStream(m_process->GetInputStream(), m_log);
  drainStream(m_process->GetErrorStream(), m_log);

  const int code = event.GetExitCode();
  appendLog(wxString::Format("\n--- Migration finished (exit code %d) ---\n", code));

  delete m_process;
  m_process = nullptr;
  m_childPid = 0;
  const bool stoppedByUser = m_stopRequested;
  m_running = false;
  m_stopRequested = false;
  setFormEnabled(true);
  m_cancelBtn->SetLabel("Close");
  m_cancelBtn->Enable(true);

  if (stoppedByUser)
  {
    appendLog("--- Migration stopped ---\n");
    return;
  }

  if (code == 0)
    wxMessageBox("Migration completed successfully.", "Done", wxOK | wxICON_INFORMATION, this);
  else
    wxMessageBox("Migration ended with an error. See output log.", "Done", wxOK | wxICON_WARNING, this);
}
