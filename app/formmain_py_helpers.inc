{$ifdef nn}begin end;{$endif}


procedure TfmMain.DoPyCommand(const AModule, AMethod: string; const AParams: TAppVariantArray);
var
  Frame: TEditorFrame;
  Ed: TATSynEdit;
  SParam: string;
begin
  if not AppPython.Inited then exit;
  PyEditorMaybeDeleted:= false;

  Frame:= CurrentFrame;
  if Frame=nil then exit;
  Ed:= Frame.Editor;
  if Ed=nil then exit;

  if Frame.MacroRecord then
  begin
    SParam:= '';
    if Length(AParams)>0 then
      SParam:= AppVariantToString(AParams[0]);
    Frame.MacroString:= Frame.MacroString+ ('py:'+AModule+','+AMethod+','+SParam+#10);
  end;

  Ed.Strings.BeginUndoGroup;
  try
    AppPython.RunCommand(AModule, AMethod, AParams);
  finally
    if not PyEditorMaybeDeleted then
      Ed.Strings.EndUndoGroup;
  end;
end;


procedure TfmMain.DoPyCommand_ByPluginIndex(AIndex: integer);
var
  F: TEditorFrame;
  CurLexer: string;
  CmdItem: TAppCommandInfo;
begin
  if not ((AIndex>=0) and (AIndex<AppCommandList.Count)) then exit;
  CmdItem:= TAppCommandInfo(AppCommandList[AIndex]);

  F:= CurrentFrame;
  if F=nil then exit;
  CurLexer:= F.LexerName[F.Editor];

  if not IsLexerListed(CurLexer, CmdItem.ItemLexers) then
  begin
    MsgStatus(msgStatusCommandOnlyForLexers+' '+CmdItem.ItemLexers);
    Exit
  end;

  if CmdItem.ItemProcParam<>'' then
    DoPyCommand(
      CmdItem.ItemModule,
      CmdItem.ItemProc,
      [AppVariant(CmdItem.ItemProcParam)]
      )
  else
    DoPyCommand(
      CmdItem.ItemModule,
      CmdItem.ItemProc,
      []
      );
end;


function TfmMain.DoPyEvent(AEd: TATSynEdit; AEvent: TAppPyEvent;
  const AParams: TAppVariantArray): TAppPyEventResult;
const
  cTheseEventsStoppedByTrue = [cEventOnComplete{, cEventOnTabSwitch}];
  cTheseEventsNeedGroupingUndo = [cEventOnComplete, cEventOnSnippet];
var
  SCurLexer, KeyForFilter: string;
  bNeedGroup: boolean;
  Frame: TEditorFrame;
  MaxPriority, NPlugin, NPriority: integer;
  Plugin: TAppEventInfo;
  bLazy: boolean;
begin
  Result.Val:= evrOther;
  Result.Str:= '';
  if not AppPython.Inited then exit;

  //block all events fired in FormCreate
  if not FHandledOnShowPartly then exit;
  //block on_focus/on_lexer fired too early
  if (AEvent in [cEventOnFocus, cEventOnLexer]) and not FHandledUntilFirstFocus then exit;

  //-1 if no such plugins
  MaxPriority:= AppEventsMaxPriorities[AEvent];
  if MaxPriority<0 then exit;

  if Assigned(AEd) then
  begin
    Frame:= GetEditorFrame(AEd);
    if Frame=nil then exit;
    SCurLexer:= Frame.LexerName[AEd];
    bNeedGroup:= AEvent in cTheseEventsNeedGroupingUndo;
  end
  else
  begin
    Frame:= nil;
    SCurLexer:= '';
    bNeedGroup:= false;
  end;

  KeyForFilter:= '';
  case AEvent of
    cEventOnKey:
      begin
        //key code is 1st int parameter
        KeyForFilter:= IntToStr(AParams[0].Int);
      end;
    cEventOnOpen:
      begin
        //filename is attached to editor's frame
        if Assigned(Frame) then
          KeyForFilter:= Copy(ExtractFileExt(Frame.GetFileName(AEd)), 2, MaxInt);
      end;
    cEventOnOpenBefore:
      begin
        //filename is 1st parameter
        KeyForFilter:= Copy(ExtractFileExt(AParams[0].Str), 2, MaxInt);
      end;
  end;

  //see all items with priority=MaxPriority..0
  for NPriority:= MaxPriority downto 0 do
    for NPlugin:= 0 to AppEventList.Count-1 do
      begin
        Plugin:= TAppEventInfo(AppEventList[NPlugin]);
        if not (AEvent in Plugin.ItemEvents) then Continue;
        if (NPriority<>Plugin.ItemEventsPrior[AEvent]) then Continue;
        if not ((Plugin.ItemLexers='') or IsLexerListed(SCurLexer, Plugin.ItemLexers)) then Continue;

        //check that OnKey event is called for supported keys
        if KeyForFilter<>'' then
          if (Plugin.ItemKeys<>'') and not IsLexerListed(KeyForFilter, Plugin.ItemKeys) then
            Continue;

        //call Python
        if bNeedGroup then
          AEd.Strings.BeginUndoGroup;

        try
          bLazy:= (AEvent=cEventOnExit) or Plugin.ItemEventsLazy[AEvent]; //on_exit must be lazy
          Result:= AppPython.RunEvent(
              Plugin.ItemModule,
              cAppPyEvent[AEvent],
              AEd,
              AParams,
              bLazy
              );
        finally
          if bNeedGroup then
            AEd.Strings.EndUndoGroup;
        end;

        //True for some events means "stop"
        if Result.Val=evrTrue then
          if AEvent in cTheseEventsStoppedByTrue then Exit;

        //False means "stop", other results ignored
        if Result.Val=evrFalse then Exit;
      end;
end;


function EditorTextToPyObject(Ed: TATSynEdit): PPyObject;
var
  Count, i: integer;
  Strs: TATStrings;
begin
  Strs:= Ed.Strings;
  Count:= Strs.Count;
  with AppPython.Engine do
  begin
    Result:= PyList_New(Count);
    for i:= 0 to Count-1 do
    begin
      PyList_SetItem(Result, i, PyUnicode_FromWideString(Strs.Lines[i]));
    end;
  end;
end;


function TfmMain.DoPyTreeHelper(Frame: TEditorFrame): boolean;
var
  Ed: TATSynEdit;
  CurLexer, CurFilename: string;
  TreeData: PPyObject;
  ParamFilename, ParamText: PPyObject;
  NHelper: integer;
  Helper: TAppTreeHelper;
begin
  Result:= false;
  if not AppPython.Inited then exit;

  Ed:= Frame.Editor;
  CurLexer:= Frame.LexerName[Ed];
  if CurLexer='' then exit;
  CurFilename:= Frame.GetFileName(Ed);

  with AppPython.Engine do
    for NHelper:= 0 to AppTreeHelpers.Count-1 do
    begin
      Helper:= TAppTreeHelper(AppTreeHelpers[NHelper]);
      if not IsLexerListed(CurLexer, Helper.ItemLexers) then Continue;

      ParamText:= EditorTextToPyObject(Ed);
      ParamFilename:= PyString_FromString(PChar(CurFilename));

      try
        TreeData:= AppPython.RunModuleFunction(Helper.ItemModule, Helper.ItemProc, [ParamFilename, ParamText]);
        try
          //refresh CodeTree only if TreeData is list
          if PyObject_TypeCheck(TreeData, PyList_Type) then
            DoCodetree_ApplyTreeHelperResults(TreeData);
        finally
          Py_DECREF(TreeData);
        end;
      except
      end;

      //Py_DECREF(ParamText);
      //Py_DECREF(ParamFilename);
      exit(true);
    end;
end;


function DoPyCallbackFromAPI(const ACallback: string;
  const AParamVars: TAppVariantArray;
  const AParamNames: array of string): boolean;
const
  cRegex_DotCommand = '([a-z_]\w*)\.([a-z_]\w*)$';
  cRegex_SignCommand = 'module=(.+?);cmd=(.+?);(info=(.+?);)?$';
  cRegex_SignFunc = 'module=(.+?);func=(.+?);(info=(.+?);)?$';
  cRegex_CommaCommand = '([a-z_]\w*),([a-z_]\w*)(,(.+))?$'; //compatible with menu_proc, with 3rd param
var
  FParamObjs: array of PPyObject;
  FParamNames: array of string;
  FParamVars: TAppVariantArray;
  SModule, SFunc, SParam, SInfo: string;
  RegexParts: TRegexParts;
  Obj: PPyObject;
  i: integer;
begin
  Result:= true;
  if not AppPython.Inited then exit;

  //avoid log error on standard ids
  if SBeginsWith(ACallback, 'top-') then exit;

  //-----------------------------------------------------
  if SRegexFindParts(cRegex_DotCommand, ACallback, RegexParts) then
  begin
    SModule:= RegexParts[1].Str;
    SFunc:= RegexParts[2].Str;

    Result:= AppPython.RunCommand(SModule, SFunc, AParamVars);
    exit;
  end;

  //-----------------------------------------------------
  if SRegexFindParts(cRegex_SignCommand, ACallback, RegexParts) then
  begin
    SModule:= RegexParts[1].Str;
    SFunc:= RegexParts[2].Str;
    SInfo:= RegexParts[4].Str;

    if SInfo<>'' then
    begin
      SetLength(FParamVars, Length(AParamVars)+1);
      for i:= 0 to Length(AParamVars)-1 do
        FParamVars[i]:= AParamVars[i];
      FParamVars[Length(FParamVars)-1]:= AppVariant(SInfo);

      Result:= AppPython.RunCommand(SModule, SFunc, FParamVars);
    end
    else
      Result:= AppPython.RunCommand(SModule, SFunc, AParamVars);

    exit;
  end;

  //-----------------------------------------------------
  if SRegexFindParts(cRegex_SignFunc, ACallback, RegexParts) then
  begin
    SModule:= RegexParts[1].Str;
    SFunc:= RegexParts[2].Str;
    SInfo:= RegexParts[4].Str;

    SetLength(FParamObjs, Length(AParamVars));
    for i:= 0 to Length(AParamVars)-1 do
      FParamObjs[i]:= AppVariantToPyObject(AParamVars[i]);

    SetLength(FParamNames, Length(AParamNames));
    for i:= 0 to Length(AParamNames)-1 do
      FParamNames[i]:= AParamNames[i];

    if SInfo<>'' then
    begin
      // SInfo may have simple type values: string, int, bool...
      SetLength(FParamObjs, Length(FParamObjs)+1);
      FParamObjs[Length(FParamObjs)-1]:= AppPython.ValueFromString(SInfo);
      SetLength(FParamNames, Length(FParamNames)+1);
      FParamNames[Length(FParamNames)-1]:= 'info';
    end;

    Obj:= AppPython.RunModuleFunction(SModule, SFunc, FParamObjs, FParamNames);
    if Assigned(Obj) then
      with AppPython.Engine do
      begin
        //only check for False
        Result:= Pointer(Obj)<>Pointer(Py_False);
        Py_DECREF(Obj);
      end;

    exit;
  end;

  //-----------------------------------------------------
  if SRegexFindParts(cRegex_CommaCommand, ACallback, RegexParts) then
  begin
    SModule:= RegexParts[1].Str;
    SFunc:= RegexParts[2].Str;
    SParam:= RegexParts[4].Str;

    if SParam<>'' then
      Result:= AppPython.RunCommand(SModule, SFunc, [AppVariant(SParam)])
    else
      Result:= AppPython.RunCommand(SModule, SFunc, []);

    MsgLogConsole(Format(msgCallbackDeprecated, [ACallback]));
    exit;
  end;

  MsgLogConsole(Format(msgCallbackBad, [ACallback]));
end;


procedure TfmMain.DoPyResetPlugins;
var
  fn, Cmd: string;
  L: TStringList;
begin
  fn:= AppDir_Py+DirectorySeparator+'cudatext_reset_plugins.py';
  if not FileExists(fn) then
  begin
    MsgBox(msgCannotFindFile+#10+fn, MB_OK or MB_ICONERROR);
    Exit
  end;

  L:= TStringList.Create;
  try
    Cmd:= Format('_reset_plugins(r"%s")', [AppDir_Py]);
    L.LoadFromFile(fn);
    L.Add(Cmd);
    AppPython.Exec(L.Text);
  finally
    FreeAndNil(L)
  end;

  AppPython.ClearCache;

  //https://github.com/Alexey-T/CudaText/issues/1253
  DoPyEvent(nil, cEventOnStart, []);
end;

procedure TfmMain.DoPyRescanPlugins;
begin
  DoOps_LoadPlugins;
  UpdateMenuPlugins;
  DoOps_LoadKeymap(false);
  MsgStatus(msgRescannedAllPlugins);
end;

procedure TfmMain.DoPyRunLastPlugin;
begin
  if AppPython.LastCommandModule='' then exit;
  if AppPython.LastCommandParam<>'' then
    DoPyCommand(
      AppPython.LastCommandModule,
      AppPython.LastCommandMethod,
      [AppVariant(AppPython.LastCommandParam)]
      )
  else
    DoPyCommand(
      AppPython.LastCommandModule,
      AppPython.LastCommandMethod,
      []
      );
end;


procedure TfmMain.DoPyTimerTick(Sender: TObject);
var
  Timer: TTimer;
  SCallback, STag: string;
  NIndex: integer;
begin
  Timer:= Sender as TTimer;
  NIndex:= FListTimers.IndexOfObject(Timer);
  if NIndex<0 then exit;

  //if timer_start_one
  if Timer.Tag=1 then
    Timer.Enabled:= false;

  SSplitByChar(FListTimers[NIndex], '|', SCallback, STag);

  DoPyCallbackFromAPI(
    SCallback,
    [AppVariant(StrToInt64Def(STag, 0))],
    ['tag']
    );
end;


