; Prince of Persia: The Revived Sands - мастер установки
; Сборка: Inno Setup 6 (jrsoftware.org). Откройте файл и нажмите Build > Compile или запустите build_setup.bat
; Нужны рядом: RevivedSands_Installer.exe (движок), info_en.txt, info_ru.txt, after_en.txt, after_ru.txt
; Мастер сам ничего не правит: он вызывает движок с ключами --cli install / --cli restore.

#define AppName "Prince of Persia: The Revived Sands"
#define AppVer "1.1.3.11 (Beta)"

[Setup]
AppId={{6D2B1C0E-8F4A-4B7D-9C35-2E1A7F90B4D3}
AppName={#AppName}
AppVersion={#AppVer}
AppVerName={#AppName} {#AppVer}
AppPublisher=wydeka
AppPublisherURL=https://github.com/wydeka/Prince-of-Persia-Revived-Sands
AppSupportURL=https://discord.gg/HRFtZ2sNJs
DefaultDirName={autopf}\RevivedSands
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
OutputDir=Output
OutputBaseFilename=RevivedSands_Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\engine.exe
ShowLanguageDialog=yes
LanguageDetectionMethod=none

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"; InfoBeforeFile: "info_en.txt"; InfoAfterFile: "after_en.txt"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"; InfoBeforeFile: "info_ru.txt"; InfoAfterFile: "after_ru.txt"

[Tasks]
Name: "ai"; Description: "{cm:TaskAI}"; GroupDescription: "{cm:TaskGroup}"; Flags: checkablealone
Name: "ai\resetini"; Description: "{cm:TaskReset}"; GroupDescription: "{cm:TaskGroup}"; Flags: unchecked dontinheritcheck
Name: "data"; Description: "{cm:TaskData}"; GroupDescription: "{cm:TaskGroup}"; Flags: checkablealone
Name: "data\malik"; Description: "{cm:TaskMalik}"; GroupDescription: "{cm:TaskGroup}"; Flags: unchecked dontinheritcheck

[Files]
; копия движка остаётся в {app}: она нужна деинсталлятору для отката
Source: "RevivedSands_Installer.exe"; DestDir: "{app}"; DestName: "engine.exe"; Flags: ignoreversion
; рабочая копия для мастера (распаковывается во временную папку)
Source: "RevivedSands_Installer.exe"; DestDir: "{tmp}"; DestName: "engine.exe"; Flags: dontcopy

[Registry]
Root: HKLM; Subkey: "Software\RevivedSands"; ValueType: string; ValueName: "GameDir"; ValueData: "{code:GetGameDir}"; Flags: uninsdeletekey

[CustomMessages]
en.TaskGroup=What to install:
en.TaskAI=Smart enemy AI (dinput8.dll and revived.ini)
en.TaskReset=Reset revived.ini to the default settings (your previous one is saved as revived.ini.bak)
en.TaskData=Enemy and Prince timings (edits inside the .forge archives)
en.TaskMalik=Use Malik's model instead of the Prince (experimental, off by default)
en.GameTitle=Game folder
en.GameDesc=Where is Prince of Persia: The Forgotten Sands installed?
en.GameSub=The mod will be installed into this folder. Close the game before continuing.
en.GameBad=This is not the game folder: DataPC.forge, DataPC_actors.forge and DataPC_01-03.forge were not found. Choose the Prince of Persia Forgotten Sands folder.
en.MalikWarn=You enabled the experimental Malik model.%n%nIt works only on levels where Malik exists in the game; where he is absent, the game crashes.%n%nContinue?
en.NothingSelected=Nothing is selected to install. Go back and tick at least one part.
en.InstallFailed=The installation was not completed.
en.RestoreFailed=The game files could not be restored:%n%n%1%n%nClose the game and try again, or choose Yes to remove the program anyway (then use Steam > Properties > Verify integrity of game files).%n%nRemove anyway?
ru.TaskGroup=Что установить:
ru.TaskAI=Умный ИИ врагов (dinput8.dll и revived.ini)
ru.TaskReset=Сбросить revived.ini к стандартным настройкам (ваши прежние сохранятся как revived.ini.bak)
ru.TaskData=Тайминги врагов и Принца (правки в архивах .forge)
ru.TaskMalik=Модель Малика вместо Принца (экспериментально, по умолчанию выключено)
ru.GameTitle=Папка игры
ru.GameDesc=Где установлена Prince of Persia: The Forgotten Sands?
ru.GameSub=Мод будет установлен в эту папку. Перед продолжением закройте игру.
ru.GameBad=Это не папка игры: не найдены DataPC.forge, DataPC_actors.forge и DataPC_01-03.forge. Выберите папку Prince of Persia Forgotten Sands.
ru.MalikWarn=Вы включили экспериментальную модель Малика.%n%nОна работает только на уровнях, где в игре есть персонаж Малик; там, где его нет, игра вылетает.%n%nПродолжить?
ru.NothingSelected=Ничего не выбрано для установки. Вернитесь назад и отметьте хотя бы одну часть.
ru.InstallFailed=Установка не завершена.
ru.RestoreFailed=Не удалось вернуть файлы игры:%n%n%1%n%nЗакройте игру и повторите, либо нажмите «Да», чтобы всё равно удалить программу (потом: Steam > Свойства > Проверить целостность файлов игры).%n%nУдалить всё равно?

[Code]
var
  GamePage: TInputDirWizardPage;
  EngineTmp: String;

function GetGameDir(Param: String): String;
begin
  Result := RemoveBackslash(GamePage.Values[0]);
end;

// последняя непустая строка файла, который пишет движок (--logfile)
function LastLine(const FileName: String): String;
var
  L: TArrayOfString;
  i: Integer;
begin
  Result := '';
  if LoadStringsFromFile(FileName, L) then
    for i := GetArrayLength(L) - 1 downto 0 do
      if Trim(L[i]) <> '' then
      begin
        Result := Trim(L[i]);
        Exit;
      end;
end;

function RunEngine(const Exe, Args, LogFile: String; var Code: Integer): Boolean;
begin
  Code := 1;
  DeleteFile(LogFile);
  Result := Exec(Exe, Args + ' --logfile "' + LogFile + '"', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure InitializeWizard;
var
  Dir, Log: String;
  Code: Integer;
begin
  ExtractTemporaryFile('engine.exe');
  EngineTmp := ExpandConstant('{tmp}\engine.exe');
  GamePage := CreateInputDirPage(wpInfoBefore, CustomMessage('GameTitle'), CustomMessage('GameDesc'), CustomMessage('GameSub'), False, '');
  GamePage.Add('');
  // тихая установка: /GameDir="D:\Games\Prince of Persia Forgotten Sands"
  Dir := ExpandConstant('{param:GameDir|}');
  if Dir = '' then
  begin
    Log := ExpandConstant('{tmp}\find.txt');
    if RunEngine(EngineTmp, '--cli find', Log, Code) and (Code = 0) then
      Dir := LastLine(Log);
  end;
  if Dir = '' then
    Dir := ExpandConstant('{commonpf32}\Steam\steamapps\common\Prince of Persia Forgotten Sands');
  GamePage.Values[0] := Dir;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  D: String;
begin
  Result := True;
  if CurPageID = GamePage.ID then
  begin
    D := RemoveBackslash(GamePage.Values[0]);
    if not (FileExists(D + '\DataPC.forge') and FileExists(D + '\DataPC_actors.forge') and FileExists(D + '\DataPC_01-03.forge')) then
    begin
      MsgBox(CustomMessage('GameBad'), mbError, MB_OK);
      Result := False;
    end;
  end
  else if CurPageID = wpSelectTasks then
  begin
    if WizardIsTaskSelected('data') and WizardIsTaskSelected('data\malik') then
      if MsgBox(CustomMessage('MalikWarn'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDNO then
        Result := False;
  end;
end;

// Движок отрабатывает ДО копирования файлов мастера: если он отказал (игра запущена, архивы не оригинальные),
// установка отменяется и в системе ничего не регистрируется.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Args, Log: String;
  Code: Integer;
  DoAI, DoData: Boolean;
begin
  Result := '';
  DoAI := WizardIsTaskSelected('ai');
  DoData := WizardIsTaskSelected('data');
  if (not DoAI) and (not DoData) then
  begin
    Result := CustomMessage('NothingSelected');
    Exit;
  end;
  Args := '--cli install --game "' + GetGameDir('') + '"';
  if not DoAI then Args := Args + ' --no-ai';
  if not DoData then Args := Args + ' --no-data';
  if DoData and WizardIsTaskSelected('data\malik') then Args := Args + ' --malik';
  if DoAI and WizardIsTaskSelected('ai\resetini') then Args := Args + ' --reset-ini';
  Log := ExpandConstant('{tmp}\install.txt');
  if (not RunEngine(EngineTmp, Args, Log, Code)) or (Code <> 0) then
    Result := CustomMessage('InstallFailed') + #13#10#13#10 + LastLine(Log);
end;

// Удаление программы = откат: сначала возвращаем файлы игры, потом убираем сам мастер.
function InitializeUninstall(): Boolean;
var
  Dir, Log: String;
  Code: Integer;
begin
  Result := True;
  if not RegQueryStringValue(HKLM, 'Software\RevivedSands', 'GameDir', Dir) then Exit;
  if not DirExists(Dir) then Exit;
  Log := ExpandConstant('{tmp}\uninstall.txt');
  if (not RunEngine(ExpandConstant('{app}\engine.exe'), '--cli restore --game "' + Dir + '"', Log, Code)) or (Code <> 0) then
    Result := MsgBox(FmtMessage(CustomMessage('RestoreFailed'), [LastLine(Log)]), mbError, MB_YESNO or MB_DEFBUTTON2) = IDYES;
end;
