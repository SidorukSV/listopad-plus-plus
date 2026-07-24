param(
  [Parameter(Mandatory = $true)]
  [string]$Executable,
  [string]$ArtifactsDirectory = (Join-Path $PSScriptRoot '..\build\gui-smoke')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class ListopadSmokeNative {
  [StructLayout(LayoutKind.Sequential)]
  public struct Rect {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
  }

  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern IntPtr SendMessage(
      IntPtr window, uint message, IntPtr wparam, IntPtr lparam);

  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool PostMessage(
      IntPtr window, uint message, IntPtr wparam, IntPtr lparam);

  [DllImport("user32.dll")]
  public static extern IntPtr GetDlgItem(IntPtr parent, int id);

  [DllImport("user32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool IsWindowVisible(IntPtr window);

  [DllImport("user32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool GetWindowRect(IntPtr window, out Rect rect);

  [DllImport("user32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool GetClientRect(IntPtr window, out Rect rect);

  [DllImport("user32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool PrintWindow(
      IntPtr window, IntPtr deviceContext, uint flags);

  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int GetClassName(
      IntPtr window, System.Text.StringBuilder name, int maximum);

  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool SetWindowText(IntPtr window, string text);

  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int GetWindowText(
      IntPtr window, System.Text.StringBuilder text, int maximum);

  [DllImport("user32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool SetForegroundWindow(IntPtr window);

  [DllImport("user32.dll")]
  public static extern IntPtr GetForegroundWindow();

  [DllImport("user32.dll")]
  public static extern uint GetWindowThreadProcessId(
      IntPtr window, out uint processId);

  [DllImport("kernel32.dll")]
  public static extern uint GetCurrentThreadId();

  [DllImport("user32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool AttachThreadInput(
      uint firstThread, uint secondThread, bool attach);

  [DllImport("user32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool BringWindowToTop(IntPtr window);

  [DllImport("user32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool ShowWindow(IntPtr window, int command);

  [DllImport("user32.dll")]
  public static extern void keybd_event(
      byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);

  [DllImport("kernel32.dll")]
  public static extern IntPtr OpenProcess(
      uint desiredAccess, bool inheritHandle, uint processId);

  [DllImport("kernel32.dll")]
  public static extern IntPtr VirtualAllocEx(
      IntPtr process, IntPtr address, UIntPtr size,
      uint allocationType, uint protection);

  [DllImport("kernel32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool VirtualFreeEx(
      IntPtr process, IntPtr address, UIntPtr size, uint freeType);

  [DllImport("kernel32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool WriteProcessMemory(
      IntPtr process, IntPtr address, byte[] buffer,
      UIntPtr size, out UIntPtr written);

  [DllImport("kernel32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool ReadProcessMemory(
      IntPtr process, IntPtr address, byte[] buffer,
      UIntPtr size, out UIntPtr read);

  [DllImport("kernel32.dll")]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool CloseHandle(IntPtr handle);
}
'@

$WM_CLOSE = 0x0010
$WM_KEYDOWN = 0x0100
$WM_KEYUP = 0x0101
$WM_LBUTTONDOWN = 0x0201
$WM_LBUTTONUP = 0x0202
$GUI_SMOKE_COMMAND = 0x803F
$VK_CONTROL = 0x11
$VK_F = 0x46
$VK_H = 0x48
$VK_TAB = 0x09
$KEYEVENTF_KEYUP = 0x0002

$IDM_FILE_SAVE = 1003
$IDM_EDIT_UNDO = 1101
$IDM_EDIT_REDO = 1102
$IDM_SEARCH_FIND = 1201
$IDM_SEARCH_REPLACE = 1202
$IDM_TOOLS_FORMAT = 1301
$IDM_VIEW_HEX = 1403
$IDC_EDITOR = 2002
$IDC_STATUS = 2003
$IDC_SEARCH_PANEL = 2007
$IDC_FIND_TEXT = 2008
$IDC_REPLACE_TEXT = 2009
$IDC_FIND_NEXT = 2010
$IDC_TOOLBAR = 2019
$IDC_DOCUMENT_MAP = 2020
$IDC_FIND_ALL = 2021
$IDC_SEARCH_RESULTS = 2022

$SCI_SELECTALL = 2013
$SCI_GOTOPOS = 2025
$SCI_GETSELECTIONSTART = 2143
$SCI_GETFIRSTVISIBLELINE = 2152
$SCI_GETLINECOUNT = 2154
$SCI_REPLACESEL = 2170
$SCI_CANUNDO = 2174
$SCI_CANREDO = 2016
$SCI_GETTEXT = 2182
$SCI_GETTEXTLENGTH = 2183
$LB_GETCOUNT = 0x018B

$PROCESS_VM_OPERATION = 0x0008
$PROCESS_VM_READ = 0x0010
$PROCESS_VM_WRITE = 0x0020
$MEM_COMMIT = 0x1000
$MEM_RESERVE = 0x2000
$MEM_RELEASE = 0x8000
$PAGE_READWRITE = 0x04

function Assert-Smoke([bool]$Condition, [string]$Message) {
  if (-not $Condition) {
    throw "GUI smoke failed: $Message"
  }
}

function Send-Command([IntPtr]$Window, [int]$Command) {
  # The command handler performs the state transition synchronously on the UI
  # thread. Waiting for SendMessage avoids races between a posted command and
  # the assertion for the next scenario step.
  [void][ListopadSmokeNative]::SendMessage(
      $Window, $script:GUI_SMOKE_COMMAND,
      [IntPtr]$Command, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 100
}

function Invoke-FindNextAndWait(
    [IntPtr]$Window, [IntPtr]$Editor, [int]$PreviousStart) {
  Send-Command $Window $script:IDC_FIND_NEXT
  foreach ($attempt in 1..50) {
    $start = [int][ListopadSmokeNative]::SendMessage(
        $Editor, $script:SCI_GETSELECTIONSTART,
        [IntPtr]::Zero, [IntPtr]::Zero)
    if ($start -ne $PreviousStart) { return $start }
    Start-Sleep -Milliseconds 100
  }
  return $PreviousStart
}

function Get-Control([IntPtr]$Window, [int]$Id, [string]$Name) {
  $control = [ListopadSmokeNative]::GetDlgItem($Window, $Id)
  Assert-Smoke ($control -ne [IntPtr]::Zero) "$Name control was not created"
  return $control
}

function Get-ControlClass([IntPtr]$Window) {
  $name = [System.Text.StringBuilder]::new(128)
  [void][ListopadSmokeNative]::GetClassName($Window, $name, $name.Capacity)
  return $name.ToString()
}

function Get-ControlText([IntPtr]$Window) {
  $text = [System.Text.StringBuilder]::new(4096)
  [void][ListopadSmokeNative]::GetWindowText(
      $Window, $text, $text.Capacity)
  return $text.ToString()
}

function Assert-VisibleControl(
    [IntPtr]$Window, [int]$Id, [string]$Name) {
  $control = Get-Control $Window $Id $Name
  Assert-Smoke ([ListopadSmokeNative]::IsWindowVisible($control)) "$Name is hidden"
  $rect = [ListopadSmokeNative+Rect]::new()
  Assert-Smoke (
      [ListopadSmokeNative]::GetWindowRect($control, [ref]$rect)) `
      "$Name rectangle is unavailable"
  Assert-Smoke (
      ($rect.Right - $rect.Left) -gt 0 -and
      ($rect.Bottom - $rect.Top) -gt 0) "$Name has an empty rectangle"
  return $control
}

function Send-Shortcut([IntPtr]$Window, [byte]$Key) {
  $currentThread = [ListopadSmokeNative]::GetCurrentThreadId()
  $targetProcess = [uint32]0
  $targetThread = [ListopadSmokeNative]::GetWindowThreadProcessId(
      $Window, [ref]$targetProcess)
  $foreground = [ListopadSmokeNative]::GetForegroundWindow()
  $foregroundProcess = [uint32]0
  $foregroundThread = [ListopadSmokeNative]::GetWindowThreadProcessId(
      $foreground, [ref]$foregroundProcess)
  $attachedTarget = $false
  $attachedForeground = $false
  try {
    if ($targetThread -ne 0 -and $targetThread -ne $currentThread) {
      $attachedTarget = [ListopadSmokeNative]::AttachThreadInput(
          $currentThread, $targetThread, $true)
    }
    if ($foregroundThread -ne 0 -and
        $foregroundThread -ne $currentThread -and
        $foregroundThread -ne $targetThread) {
      $attachedForeground = [ListopadSmokeNative]::AttachThreadInput(
          $currentThread, $foregroundThread, $true)
    }
    [void][ListopadSmokeNative]::ShowWindow($Window, 9)
    [void][ListopadSmokeNative]::BringWindowToTop($Window)
    [void][ListopadSmokeNative]::SetForegroundWindow($Window)
  } finally {
    if ($attachedForeground) {
      [void][ListopadSmokeNative]::AttachThreadInput(
          $currentThread, $foregroundThread, $false)
    }
    if ($attachedTarget) {
      [void][ListopadSmokeNative]::AttachThreadInput(
          $currentThread, $targetThread, $false)
    }
  }
  Start-Sleep -Milliseconds 150
  foreach ($attempt in 1..10) {
    if ([ListopadSmokeNative]::GetForegroundWindow() -eq $Window) { break }
    [void][ListopadSmokeNative]::ShowWindow($Window, 9)
    [void][ListopadSmokeNative]::BringWindowToTop($Window)
    [void][ListopadSmokeNative]::SetForegroundWindow($Window)
    Start-Sleep -Milliseconds 100
  }
  if ([ListopadSmokeNative]::GetForegroundWindow() -ne $Window) {
    return $false
  }
  [ListopadSmokeNative]::keybd_event(
      $script:VK_CONTROL, 0, 0, [UIntPtr]::Zero)
  [ListopadSmokeNative]::keybd_event($Key, 0, 0, [UIntPtr]::Zero)
  [ListopadSmokeNative]::keybd_event(
      $Key, 0, $script:KEYEVENTF_KEYUP, [UIntPtr]::Zero)
  [ListopadSmokeNative]::keybd_event(
      $script:VK_CONTROL, 0, $script:KEYEVENTF_KEYUP, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 300
  return $true
}

function Open-EditorProcess([uint32]$ProcessId) {
  $access = $script:PROCESS_VM_OPERATION -bor
            $script:PROCESS_VM_READ -bor
            $script:PROCESS_VM_WRITE
  $handle = [ListopadSmokeNative]::OpenProcess($access, $false, $ProcessId)
  Assert-Smoke ($handle -ne [IntPtr]::Zero) "cannot open candidate process memory"
  return $handle
}

function Invoke-WithRemoteBytes(
    [IntPtr]$ProcessHandle, [byte[]]$Bytes, [scriptblock]$Action) {
  $remote = [ListopadSmokeNative]::VirtualAllocEx(
      $ProcessHandle, [IntPtr]::Zero, [UIntPtr]$Bytes.Length,
      $script:MEM_COMMIT -bor $script:MEM_RESERVE, $script:PAGE_READWRITE)
  Assert-Smoke ($remote -ne [IntPtr]::Zero) "cannot allocate candidate memory"
  try {
    $written = [UIntPtr]::Zero
    Assert-Smoke (
        [ListopadSmokeNative]::WriteProcessMemory(
            $ProcessHandle, $remote, $Bytes, [UIntPtr]$Bytes.Length,
            [ref]$written)) "cannot write candidate memory"
    return & $Action $remote
  } finally {
    [void][ListopadSmokeNative]::VirtualFreeEx(
        $ProcessHandle, $remote, [UIntPtr]::Zero, $script:MEM_RELEASE)
  }
}

function Get-EditorText(
    [IntPtr]$Editor, [IntPtr]$ProcessHandle) {
  $length = [int][ListopadSmokeNative]::SendMessage(
      $Editor, $script:SCI_GETTEXTLENGTH, [IntPtr]::Zero, [IntPtr]::Zero)
  $buffer = [byte[]]::new($length + 1)
  $remote = [ListopadSmokeNative]::VirtualAllocEx(
      $ProcessHandle, [IntPtr]::Zero, [UIntPtr]$buffer.Length,
      $script:MEM_COMMIT -bor $script:MEM_RESERVE, $script:PAGE_READWRITE)
  Assert-Smoke ($remote -ne [IntPtr]::Zero) "cannot allocate text buffer"
  try {
    [void][ListopadSmokeNative]::SendMessage(
        $Editor, $script:SCI_GETTEXT, [IntPtr]$buffer.Length, $remote)
    $read = [UIntPtr]::Zero
    Assert-Smoke (
        [ListopadSmokeNative]::ReadProcessMemory(
            $ProcessHandle, $remote, $buffer, [UIntPtr]$buffer.Length,
            [ref]$read)) "cannot read editor text"
    return [System.Text.Encoding]::UTF8.GetString($buffer, 0, $length)
  } finally {
    [void][ListopadSmokeNative]::VirtualFreeEx(
        $ProcessHandle, $remote, [UIntPtr]::Zero, $script:MEM_RELEASE)
  }
}

function Replace-EditorText(
    [IntPtr]$Editor, [IntPtr]$ProcessHandle, [string]$Text) {
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text + [char]0)
  [void][ListopadSmokeNative]::SendMessage(
      $Editor, $script:SCI_SELECTALL, [IntPtr]::Zero, [IntPtr]::Zero)
  Invoke-WithRemoteBytes $ProcessHandle $bytes {
    param([IntPtr]$remote)
    [void][ListopadSmokeNative]::SendMessage(
        $Editor, $script:SCI_REPLACESEL, [IntPtr]::Zero, $remote)
  }
  [void][ListopadSmokeNative]::SendMessage(
      $Editor, $script:SCI_GOTOPOS,
      [IntPtr][System.Text.Encoding]::UTF8.GetByteCount($Text),
      [IntPtr]::Zero)
}

function Expand-Emmet(
    [IntPtr]$Editor, [IntPtr]$ProcessHandle, [string]$Abbreviation) {
  Replace-EditorText $Editor $ProcessHandle $Abbreviation
  [void][ListopadSmokeNative]::SendMessage(
      $Editor, $script:WM_KEYDOWN, [IntPtr]$script:VK_TAB, [IntPtr]1)
  [void][ListopadSmokeNative]::SendMessage(
      $Editor, $script:WM_KEYUP, [IntPtr]$script:VK_TAB, [IntPtr]1)
  Start-Sleep -Milliseconds 150
  return Get-EditorText $Editor $ProcessHandle
}

function Save-WindowScreenshot([IntPtr]$Window, [string]$Path) {
  $rect = [ListopadSmokeNative+Rect]::new()
  Assert-Smoke (
      [ListopadSmokeNative]::GetWindowRect($Window, [ref]$rect)) `
      "main window rectangle is unavailable"
  $width = $rect.Right - $rect.Left
  $height = $rect.Bottom - $rect.Top
  $bitmap = [System.Drawing.Bitmap]::new($width, $height)
  try {
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
      $deviceContext = $graphics.GetHdc()
      try {
        Assert-Smoke (
            [ListopadSmokeNative]::PrintWindow(
                $Window, $deviceContext, 2)) 'PrintWindow failed'
      } finally {
        $graphics.ReleaseHdc($deviceContext)
      }
    } finally {
      $graphics.Dispose()
    }
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
  } finally {
    $bitmap.Dispose()
  }
}

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
[System.IO.Directory]::CreateDirectory($ArtifactsDirectory) | Out-Null
$resolvedArtifacts = (Resolve-Path -LiteralPath $ArtifactsDirectory).Path
$runId = [Guid]::NewGuid().ToString('N')
$profileDirectory = Join-Path ([System.IO.Path]::GetTempPath()) "ListopadPP-profile-$runId"
$documentPath = Join-Path ([System.IO.Path]::GetTempPath()) "ListopadPP-smoke-$runId.html"
$searchScreenshot = Join-Path $resolvedArtifacts 'search-panel.png'
$formatScreenshot = Join-Path $resolvedArtifacts 'formatted-map.png'
$process = $null
$processHandle = [IntPtr]::Zero
$mainWindow = [IntPtr]::Zero

try {
  [System.IO.Directory]::CreateDirectory($profileDirectory) | Out-Null
  $settings = @{
    uiLanguage = 'ru'
    theme = 'dark'
    fontFace = 'Consolas'
    fontSize = 13
    indentSize = 2
    indentWithTabs = $false
    documentMap = $true
    largeFileThreshold = 67108864
    fallbackEncoding = 'windows-1251'
    window = @{
      x = 80
      y = 80
      width = 1280
      height = 820
      maximized = $false
    }
  } | ConvertTo-Json -Depth 4
  [System.IO.File]::WriteAllText(
      (Join-Path $profileDirectory 'settings.json'), $settings,
      [System.Text.UTF8Encoding]::new($false))

  $lines = [System.Collections.Generic.List[string]]::new()
  $lines.Add('<!DOCTYPE html>')
  $lines.Add('<html><head><title>GUI smoke</title></head><body>')
  foreach ($index in 1..800) {
    $lines.Add(
        "<div id=`"row-$index`"><span>Smoke line $index</span></div>")
  }
  $lines.Add('</body></html>')
  $originalText = [string]::Join("`r`n", $lines) + "`r`n"
  [System.IO.File]::WriteAllText(
      $documentPath, $originalText, [System.Text.UTF8Encoding]::new($false))

  $env:LISTOPAD_INSTANCE_ID = "gui-smoke-$runId"
  $env:LISTOPAD_PROFILE_DIR = $profileDirectory
  $env:LISTOPAD_GUI_SMOKE = '1'
  $env:LISTOPAD_GUI_SMOKE_QUERY = 'div'
  $process = Start-Process -FilePath $resolvedExecutable `
      -ArgumentList @('--', $documentPath) -PassThru

  foreach ($attempt in 1..100) {
    if ($process.HasExited) {
      throw "Candidate exited with code $($process.ExitCode)"
    }
    $process.Refresh()
    if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
      $mainWindow = $process.MainWindowHandle
      break
    }
    Start-Sleep -Milliseconds 100
  }
  Assert-Smoke ($mainWindow -ne [IntPtr]::Zero) "main window did not appear"
  $mainWindowProcess = [uint32]0
  [void][ListopadSmokeNative]::GetWindowThreadProcessId(
      $mainWindow, [ref]$mainWindowProcess)
  Assert-Smoke ($mainWindowProcess -eq [uint32]$process.Id) `
      "main window belongs to process $mainWindowProcess instead of $($process.Id)"
  $processHandle = Open-EditorProcess ([uint32]$process.Id)

  $editor = Assert-VisibleControl $mainWindow $IDC_EDITOR 'editor'
  Assert-Smoke ((Get-ControlClass $editor) -eq 'Scintilla') `
      "text editor class is not Scintilla"
  $toolbar = Assert-VisibleControl $mainWindow $IDC_TOOLBAR 'toolbar'
  $map = Assert-VisibleControl $mainWindow $IDC_DOCUMENT_MAP 'document map'
  Assert-Smoke ((Get-ControlClass $map) -eq 'ListopadPPDocumentMap') `
      "document map class is unexpected"
  Assert-Smoke ((Get-EditorText $editor $processHandle) -eq $originalText) `
      "opened file contents differ from disk"

  $ctrlFKeyboard = Send-Shortcut $mainWindow $VK_F
  if (-not $ctrlFKeyboard) {
    Send-Command $mainWindow $IDM_SEARCH_FIND
  }
  [void](Assert-VisibleControl $mainWindow $IDC_TOOLBAR 'toolbar after Ctrl+F')
  [void](Assert-VisibleControl $mainWindow $IDC_SEARCH_PANEL 'search panel')
  $findText = Assert-VisibleControl $mainWindow $IDC_FIND_TEXT 'find field'
  Assert-Smoke (
      [ListopadSmokeNative]::SetWindowText($findText, 'div')) `
      "cannot set find query"
  $confirmedQuery = Get-ControlText $findText
  Assert-Smoke ($confirmedQuery -eq 'div') `
      "find field contains '$confirmedQuery' instead of 'div'"
  [void][ListopadSmokeNative]::SendMessage(
      $editor, $SCI_GOTOPOS, [IntPtr]::Zero, [IntPtr]::Zero)
  $firstMatch = Invoke-FindNextAndWait $mainWindow $editor 0
  $secondMatch = Invoke-FindNextAndWait $mainWindow $editor $firstMatch
  $queryAfterSearch = Get-ControlText $findText
  $statusControl = Get-Control $mainWindow $IDC_STATUS 'status bar'
  $searchStatus = Get-ControlText $statusControl
  if ($secondMatch -le $firstMatch) {
    Save-WindowScreenshot $mainWindow $searchScreenshot
  }
  Assert-Smoke ($secondMatch -gt $firstMatch) `
      "Find next stayed on the first occurrence (first=$firstMatch, second=$secondMatch, query='$queryAfterSearch', status='$searchStatus')"

  $findAllButton = Get-Control $mainWindow $IDC_FIND_ALL 'find-all button'
  Send-Command $mainWindow $IDC_FIND_ALL
  $results = Get-Control $mainWindow $IDC_SEARCH_RESULTS 'find-all results'
  $resultCount = 0
  foreach ($attempt in 1..50) {
    $resultCount = [int][ListopadSmokeNative]::SendMessage(
        $results, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($resultCount -gt 0) { break }
    Start-Sleep -Milliseconds 100
  }
  Assert-Smoke ($resultCount -ge 800) `
      "Find all returned $resultCount results instead of at least 800"
  [void](Assert-VisibleControl $mainWindow $IDC_SEARCH_RESULTS 'find-all results')
  Save-WindowScreenshot $mainWindow $searchScreenshot

  $ctrlHKeyboard = Send-Shortcut $mainWindow $VK_H
  if (-not $ctrlHKeyboard) {
    Send-Command $mainWindow $IDM_SEARCH_REPLACE
  }
  [void](Assert-VisibleControl $mainWindow $IDC_TOOLBAR 'toolbar after Ctrl+H')
  [void](Assert-VisibleControl $mainWindow $IDC_REPLACE_TEXT 'replace field')
  [void](Assert-VisibleControl $mainWindow $IDC_DOCUMENT_MAP 'map after Ctrl+H')

  $mapRect = [ListopadSmokeNative+Rect]::new()
  [void][ListopadSmokeNative]::GetClientRect($map, [ref]$mapRect)
  $mapX = [Math]::Max(1, [int](($mapRect.Right - $mapRect.Left) / 2))
  $mapY = [Math]::Max(1, $mapRect.Bottom - 2)
  $mapPoint = [IntPtr](($mapY -shl 16) -bor ($mapX -band 0xffff))
  [void][ListopadSmokeNative]::SendMessage(
      $map, $WM_LBUTTONDOWN, [IntPtr]1, $mapPoint)
  [void][ListopadSmokeNative]::SendMessage(
      $map, $WM_LBUTTONUP, [IntPtr]::Zero, $mapPoint)
  Start-Sleep -Milliseconds 250
  $lineCount = [int][ListopadSmokeNative]::SendMessage(
      $editor, $SCI_GETLINECOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
  $firstVisibleLine = [int][ListopadSmokeNative]::SendMessage(
      $editor, $SCI_GETFIRSTVISIBLELINE, [IntPtr]::Zero, [IntPtr]::Zero)
  Assert-Smoke ($firstVisibleLine -gt [int]($lineCount / 2)) `
      "map tail click navigated to line $firstVisibleLine of $lineCount"

  Send-Command $mainWindow $IDM_TOOLS_FORMAT
  $formattedText = Get-EditorText $editor $processHandle
  Assert-Smoke ($formattedText -ne $originalText) 'Format did not change the document'
  Assert-Smoke (
      [ListopadSmokeNative]::SendMessage(
          $editor, $SCI_CANUNDO, [IntPtr]::Zero, [IntPtr]::Zero) -ne
          [IntPtr]::Zero) 'formatted document cannot be undone'
  [void](Assert-VisibleControl $mainWindow $IDC_TOOLBAR 'toolbar after Format')
  [void](Assert-VisibleControl $mainWindow $IDC_DOCUMENT_MAP 'map after Format')
  Save-WindowScreenshot $mainWindow $formatScreenshot

  Send-Command $mainWindow $IDM_FILE_SAVE
  Send-Command $mainWindow $IDM_VIEW_HEX
  $hexView = Assert-VisibleControl $mainWindow $IDC_EDITOR 'hex view'
  Assert-Smoke ((Get-ControlClass $hexView) -eq 'ListopadPPHexView') `
      'Text to Hex did not create the hex view'
  Send-Command $mainWindow $IDM_VIEW_HEX
  $restoredEditor = Assert-VisibleControl $mainWindow $IDC_EDITOR 'restored editor'
  Assert-Smoke ((Get-ControlClass $restoredEditor) -eq 'Scintilla') `
      'Hex to Text did not restore Scintilla'
  Assert-Smoke ($restoredEditor -eq $editor) `
      'Hex round-trip recreated the text surface and lost editor identity'
  Assert-Smoke (
      [ListopadSmokeNative]::SendMessage(
          $editor, $SCI_CANUNDO, [IntPtr]::Zero, [IntPtr]::Zero) -ne
          [IntPtr]::Zero) 'Hex round-trip lost Undo history'

  Send-Command $mainWindow $IDM_EDIT_UNDO
  Assert-Smoke ((Get-EditorText $editor $processHandle) -eq $originalText) `
      'Undo after Text/Hex/Text did not restore the unformatted document'
  Assert-Smoke (
      [ListopadSmokeNative]::SendMessage(
          $editor, $SCI_CANREDO, [IntPtr]::Zero, [IntPtr]::Zero) -ne
          [IntPtr]::Zero) 'Undo did not create a Redo action'
  Send-Command $mainWindow $IDM_EDIT_REDO
  Assert-Smoke ((Get-EditorText $editor $processHandle) -eq $formattedText) `
      'Redo did not restore the formatted document'
  Send-Command $mainWindow $IDM_FILE_SAVE

  $firstExpansion = Expand-Emmet $editor $processHandle 'span*3'
  Assert-Smoke (
      ([regex]::Matches($firstExpansion, '<span></span>')).Count -eq 3) `
      'first Emmet expansion failed'
  $secondExpansion = Expand-Emmet $editor $processHandle 'span*3'
  Assert-Smoke (
      ([regex]::Matches($secondExpansion, '<span></span>')).Count -eq 3) `
      'second Emmet expansion after deletion failed'
  Send-Command $mainWindow $IDM_FILE_SAVE

  [void][ListopadSmokeNative]::PostMessage(
      $mainWindow, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
  Assert-Smoke ($process.WaitForExit(5000)) 'candidate did not close cleanly'

  [pscustomobject]@{
    executable = $resolvedExecutable
    processId = $process.Id
    findNextFirst = $firstMatch
    findNextSecond = $secondMatch
    findAllResults = $resultCount
    mapFirstVisibleLine = $firstVisibleLine
    mapLineCount = $lineCount
    textSurfacePreserved = $restoredEditor -eq $editor
    ctrlFKeyboard = $ctrlFKeyboard
    ctrlHKeyboard = $ctrlHKeyboard
    searchScreenshot = $searchScreenshot
    formatScreenshot = $formatScreenshot
    status = 'passed'
  } | ConvertTo-Json
} finally {
  if ($processHandle -ne [IntPtr]::Zero) {
    [void][ListopadSmokeNative]::CloseHandle($processHandle)
  }
  if ($process -and -not $process.HasExited) {
    if ($mainWindow -ne [IntPtr]::Zero) {
      [void][ListopadSmokeNative]::PostMessage(
          $mainWindow, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
      [void]$process.WaitForExit(1500)
    }
    if (-not $process.HasExited) {
      Stop-Process -Id $process.Id -Force
    }
  }
  Remove-Item -LiteralPath $documentPath -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $profileDirectory -Recurse -Force `
      -ErrorAction SilentlyContinue
}
