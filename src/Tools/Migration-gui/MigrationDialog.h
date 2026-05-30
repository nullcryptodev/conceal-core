// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <wx/dialog.h>
#include <wx/filepicker.h>
#include <wx/process.h>
#include <wx/timer.h>

class wxButton;
class wxCheckBox;
class wxDirPickerCtrl;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

class MigrationDialog : public wxDialog
{
public:
  explicit MigrationDialog(wxWindow *parent);

private:
  enum
  {
    ID_OLD_DIR = 4001,
    ID_NEW_DIR,
    ID_TESTNET,
    ID_SKIP_VALIDATION,
    ID_BATCH_SIZE,
    ID_CREATE_DIR,
    ID_CANCEL,
    ID_MIGRATE,
    ID_POLL_TIMER,
  };

  void buildLayout();
  wxString oldDirPath() const;
  wxString newDirPath() const;
  void updateOldStatus();
  void updateNewStatus();
  bool validateInputs(wxString *error) const;
  wxArrayString buildMigrateArgv() const;
  void stopMigrationProcess();
  void setFormEnabled(bool enabled);
  void appendLog(const wxString &text);

  void onOldDirChanged(wxFileDirPickerEvent &event);
  void onNewDirChanged(wxFileDirPickerEvent &event);
  void onOldDirText(wxCommandEvent &event);
  void onNewDirText(wxCommandEvent &event);
  void onCreateDir(wxCommandEvent &event);
  void onMigrate(wxCommandEvent &event);
  void onCancel(wxCommandEvent &event);
  void onPollTimer(wxTimerEvent &event);
  void onProcessTerminated(wxProcessEvent &event);

  wxDirPickerCtrl *m_oldDirPicker;
  wxDirPickerCtrl *m_newDirPicker;
  wxStaticText *m_oldStatus;
  wxStaticText *m_newStatus;
  wxCheckBox *m_testnetCheck;
  wxCheckBox *m_skipValidationCheck;
  wxSpinCtrl *m_batchSizeSpin;
  wxTextCtrl *m_log;
  wxButton *m_migrateBtn;
  wxButton *m_cancelBtn;

  wxProcess *m_process;
  wxTimer m_pollTimer;
  long m_childPid;
  bool m_running;
  bool m_stopRequested;

  wxDECLARE_EVENT_TABLE();
};
