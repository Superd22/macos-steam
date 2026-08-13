# `window.SteamClient` on macOS — full API inventory

Captured **2026-08-13** from macOS Steam client **`1785187029`** (Apple Silicon,
macOS 26.5.2), via CDP `Runtime.evaluate` against the **`SharedJSContext`** target on
`127.0.0.1:8080`. Companion artifact to
[#6](https://github.com/Superd22/macos-steam/issues/6); method inventory generated
mechanically, not hand-transcribed.

To our knowledge no public inventory of the **macOS** client's `SteamClient` surface
exists — public references (SteamDB's typings, Millennium, decky-loader) document the
Windows/Linux/Deck clients.

## How to reproduce

1. `touch "~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/.cef-enable-remote-debugging"`
   — the **install** dir inside the app bundle, *not* the data dir (the recipe in
   `compatibilitytools-d-macos.md` is wrong about this; corrected on
   [#9](https://github.com/Superd22/macos-steam/issues/9)). The `-cef-enable-debugging`
   launch flag is an equivalent, per-run alternative.
2. Restart Steam; `curl -s http://127.0.0.1:8080/json/list` within ~15s of UI paint.
3. Pick the target with `"title": "SharedJSContext"` and speak CDP over its
   `webSocketDebuggerUrl` — `Runtime.evaluate` with `awaitPromise: true,
   returnByValue: true`.

⚠️ **Always clean up**: the port is localhost-only but is arbitrary JS execution inside
the Steam client for any local process. Remove the marker file and relaunch Steam with
no flags; confirm 8080 is closed.

## Reading this inventory

- Every method is a **variadic native binding** — `Function.length` is `0` for all
  847 of them, so arity is not recoverable by reflection. Argument shapes must come
  from observed calls (see the verified table) or from the strings in
  `steamclient.dylib`/`steamui.dylib`.
- Most methods return **Promises**. `RegisterFor*` methods take a callback and return
  `{ unregister() }`.
- Presence ≠ functionality: macOS-irrelevant namespaces (e.g. `SteamChina`,
  `OpenVR`) enumerate identically to live ones. The verified table below lists what
  has actually been driven on this machine.

## Verified functional on this machine

| Call | Verdict | Evidence |
|---|---|---|
| `SteamClient.Console.ExecCommand(cmd)` | ✅ works | `@sSteamCmdForcePlatformType windows` + `config_refresh` flipped every Windows-only title `display_status 14 → 9` ([#15](https://github.com/Superd22/macos-steam/issues/15)) |
| `SteamClient.Apps.SpecifyCompatTool(appid, tool)` | ✅ works | wrote `CompatToolMapping` at priority 250 into `config.vdf`, logged by the compat manager ([#15](https://github.com/Superd22/macos-steam/issues/15)) |
| `SteamClient.Apps.GetAvailableCompatTools(appid)` | ✅ works | returned the registered probe tool ([#5](https://github.com/Superd22/macos-steam/issues/5)) |
| `SteamClient.Settings.GetGlobalCompatTools()` | ✅ works | same ([#5](https://github.com/Superd22/macos-steam/issues/5)) |
| `SteamClient.Installs.GetInstallManagerInfo()` | ⚠️ half-works | `nDiskSpaceRequired` is **exact to the byte** for the depot set Steam would select — the zero-download depot oracle. Its `eAppError` is **worthless**: `0` even for a bogus appid ([#15](https://github.com/Superd22/macos-steam/issues/15), [#9](https://github.com/Superd22/macos-steam/issues/9)) |
| Install + launch driven headlessly via `SteamClient.*` | ✅ works | Surviving Mars installed (`SizeOnDisk` byte-identical to the oracle) and Among Us launched through a compat tool, on the patched client ([#16 follow-up](https://github.com/Superd22/macos-steam/issues/16#issuecomment-5171159254)) |

**`display_status` enum** (decoded on [#15](https://github.com/Superd22/macos-steam/issues/15)):
`6` = installed · `9` = ready to install · `14` = not available on this platform.

**State at capture time:** stock-behaving client — `GetGlobalCompatTools()` → `[]`
(the #5 probe tool is no longer registered) and
`settingsStore.settings.bCompatEnabled` → `false`. The inventory below is therefore
the *unmodified* surface, not an artifact of our patches.

---

## `window.SteamClient` — 48 namespaces, 847 methods

### `SteamClient._internal`

`BInGpuFallbackMode`, `ExecutePromise`, `GetBrowserProcessDetails`, `GetDisplayScaleFactors`, `IsDebuggingEnabled`, `RegisterForStyleChanges`, `RequestDisableGpu`, `SetDevMode`, `SetForceDeviceScaleFactor`, `SetRightToLeftMode`


### `SteamClient.Apps`

`AddShortcut`, `BackupFilesForApp`, `BrowseScreenshotForApp`, `BrowseScreenshotsForApp`, `CancelBackup`, `CancelGameAction`, `CancelLaunch`, `ClearCustomArtworkForApp`, `ClearCustomLogoPositionForApp`, `ClearProton`, `ContinueGameAction`, `CreateDesktopShortcutForApp`, `DownloadWorkshopItem`, `GetAchievementsInTimeRange`, `GetActiveGameActions`, `GetAvailableCompatTools`, `GetBackupsInFolder`, `GetCachedAppDetails`, `GetCloudPendingRemoteOperations`, `GetCompatExperiment`, `GetConflictingFileTimestamps`, `GetDetailsForScreenshotUpload`, `GetDetailsForScreenshotUploads`, `GetDownloadedWorkshopItems`, `GetDurationControlInfo`, `GetFriendAchievementsForApp`, `GetFriendsWhoPlay`, `GetGameActionDetails`, `GetGameActionForApp`, `GetIsSubscribedApp`, `GetLaunchOptionsForApp`, `GetMyAchievementsForApp`, `GetPlaytime`, `GetPrePurchasedApps`, `GetResolutionOverrideForApp`, `GetScreenshotInfo`, `GetScreenshotsInTimeRange`, `GetShortcutDataForPath`, `GetSoundtrackDetails`, `GetSubscribedWorkshopItemDetails`, `GetSubscribedWorkshopItems`, `InstallFlatpakAppAndCreateShortcut`, `JoinAppContentBeta`, `JoinAppContentBetaByPassword`, `LaunchNonSteamApp`, `ListFlatpakApps`, `LoadEula`, `MarkEulaAccepted`, `MarkEulaRejected`, `MoveWorkshopItemLoadOrder`, `OpenAppSettingsDialog`, `RaiseWindowForGame`, `RegisterForAchievementChanges`, `RegisterForAppBackupStatus`, `RegisterForAppDetails`, `RegisterForAppOverviewChanges`, `RegisterForDRMFailureResponse`, `RegisterForGameActionEnd`, `RegisterForGameActionShowError`, `RegisterForGameActionShowUI`, `RegisterForGameActionStart`, `RegisterForGameActionTaskChange`, `RegisterForGameActionUserRequest`, `RegisterForPrePurchasedAppChanges`, `RegisterForShowMarketingMessageDialog`, `RegisterForShowPendingGiftsDialog`, `RegisterForWorkshopChanges`, `RegisterForWorkshopItemDownloads`, `RegisterForWorkshopItemInstalled`, `RemoveShortcut`, `ReportLibraryAssetCacheMiss`, `ReportMarketingMessageDialogShown`, `ReportPendingGiftsDialogShown`, `RequestIconDataForApp`, `RequestLegacyCDKeysForApp`, `RunGame`, `SaveAchievementProgressCache`, `ScanForInstalledNonSteamApps`, `SetAppAutoUpdateBehavior`, `SetAppBackgroundDownloadsBehavior`, `SetAppCurrentLanguage`, `SetAppLaunchOptions`, `SetAppResolutionOverride`, `SetCachedAppDetails`, `SetControllerRumblePreference`, `SetCustomArtworkForApp`, `SetCustomLogoPositionForApp`, `SetDLCEnabled`, `SetFDMEnable`, `SetForceIdentAsSteamDeck`, `SetLocalScreenshotCaption`, `SetLocalScreenshotPrivacy`, `SetLocalScreenshotSpoiler`, `SetRPOEnable`, `SetShortcutExe`, `SetShortcutIcon`, `SetShortcutIsVR`, `SetShortcutLaunchOptions`, `SetShortcutName`, `SetShortcutSortAs`, `SetShortcutStartDir`, `SetStreamingClientForApp`, `SetTSOEnable`, `SetThirdPartyControllerConfiguration`, `SetWorkshopItemsDisabledLocally`, `SetWorkshopItemsLoadOrder`, `ShowControllerConfigurator`, `ShowStore`, `SpecifyCompatExperiment`, `SpecifyCompatTool`, `StreamGame`, `SubscribeWorkshopItem`, `TerminateApp`, `ToggleAllowDesktopConfiguration`, `ToggleAppSteamCloudEnabled`, `ToggleAppSteamCloudSyncOnSuspendEnabled`, `ToggleEnableSteamOverlayForApp`, `ToggleOverrideResolutionForInternalDisplay`, `UninstallFlatpakApp`, `VerifyApp`


### `SteamClient.Auth`

`ClearCachedSignInPin`, `CurrentUserHasCachedSignInPin`, `GetLocalHostname`, `GetMachineID`, `GetRefreshInfo`, `GetSteamGuardData`, `IsSecureComputer`, `SetCachedSignInPin`, `SetLoginToken`, `SetSteamGuardData`, `StartSignInFromCache`, `UserHasCachedSignInPin`, `ValidateCachedSignInPin`


### `SteamClient.Broadcast`

`ApproveViewerRequest`, `InviteToWatch`, `RegisterForBroadcastStatus`, `RegisterForViewerRequests`, `RejectViewerRequest`, `StopBroadcasting`


### `SteamClient.Browser`

`AddWordToDictionary`, `ClearAllBrowsingData`, `ClearHistory`, `CloseDevTools`, `GetBrowserID`, `GetSpellingSuggestions`, `GetSteamBrowserID`, `HideCursorUntilMouseEvent`, `InspectElement`, `NotifyUserActivation`, `OpenDevTools`, `Paste`, `RegisterForGestureEvents`, `RegisterForOpenNewTab`, `RemoveAllBackstackEntries`, `ReplaceMisspelling`, `RestartJSContext`, `SetBackgroundThrottlingDisabled`, `SetPendingFilePath`, `SetShouldExitSteamOnBrowserClosed`, `SetTouchGesturesToCancel`, `StartDownload`


### `SteamClient.BrowserView`

`Create`, `CreatePopup`, `Destroy`, `PostMessageToParent`


### `SteamClient.ClientNotifications`

`DisplayClientNotification`, `OnRespondToClientNotification`


### `SteamClient.Cloud`

`ResolveAppSyncConflict`, `RetryAppSync`


### `SteamClient.CloudStorage`

`WriteKey`


### `SteamClient.CommunityItems`

`DownloadItemAsset`, `GetItemAssetPath`, `RemoveDownloadedItemAsset`


### `SteamClient.Compat`

`CheckBootProtectionEnabled`


### `SteamClient.Console`

`ExecCommand`, `GetAutocompleteSuggestions`, `RegisterForSpewOutput`


### `SteamClient.Customization`

`GenerateLocalStartupMoviesThumbnails`, `GetDownloadedStartupMovies`, `GetLocalStartupMovies`


### `SteamClient.Downloads`

`EnableAllDownloads`, `MoveAppUpdateDown`, `MoveAppUpdateUp`, `PauseAppUpdate`, `QueueAppUpdate`, `RegisterForDownloadItems`, `RegisterForDownloadOverview`, `RegisterForRemoteClientsConnected`, `RemoveFromDownloadList`, `ResumeAppUpdate`, `SetLaunchOnUpdateComplete`, `SetQueueIndex`, `SuspendDownloadThrottling`, `SuspendLanPeerContent`


### `SteamClient.FamilySharing`

`GetAvailableLenders`, `RegisterForKickedBorrower`, `SetPreferredLender`


### `SteamClient.Friends`

`GetCoplayData`, `InviteUserToCurrentGame`, `InviteUserToGame`, `InviteUserToLobby`, `InviteUserToRemotePlayTogetherCurrentGame`, `RegisterForMultiplayerSessionShareURLChanged`, `RegisterForVoiceChatStatus`, `ShowRemotePlayTogetherUI`


### `SteamClient.FriendSettings`

`GetEnabledFeatures`, `RegisterForSettingsChanges`, `SetFriendSettings`


### `SteamClient.GameNotes`

`DeleteImage`, `DeleteNotes`, `GetNotes`, `GetNotesMetadata`, `GetNumNotes`, `GetQuota`, `IterateNotes`, `ResolveSyncConflicts`, `SaveNotes`, `SyncToClient`, `SyncToServer`, `UploadImage`


### `SteamClient.GameRecording`

`RegisterForAudioSessionsChanged`, `SetAudioSessionCaptureState`


### `SteamClient.GameSessions`

`RegisterForAchievementNotification`, `RegisterForAppLifetimeNotifications`, `RegisterForScreenshotNotification`


### `SteamClient.Input`

`CalibrateControllerIMU`, `CalibrateControllerJoystick`, `CalibrateControllerTrackpads`, `ClearSelectedConfigForApp`, `CloseDesktopConfigurator`, `ControllerKeyboardSendText`, `ControllerKeyboardSetKeyState`, `DecrementCloudedControllerConfigsCounter`, `DeletePersonalControllerConfiguration`, `DuplicateControllerConfigurationSourceMode`, `EnableControllerAnalogInputMessages`, `EndControllerDeviceSupportFlow`, `ExportCurrentControllerConfiguration`, `ForceConfiguratorFocus`, `ForceSimpleHapticEvent`, `FreeControllerConfig`, `GetConfigForAppAndController`, `GetControllerMappingString`, `GetControllerPreviouslySeen`, `GetTouchMenuIconsForApp`, `GetXboxDriverInstallState`, `IdentifyController`, `InitControllerSounds`, `InitializeControllerPersonalizationSettings`, `ModalKeyboardDismissed`, `OpenDesktopConfigurator`, `PreviewConfigForAppAndController`, `PreviewControllerLEDColor`, `QueryControllerConfigsForApp`, `RegisterForActiveConfigLoadedMessages`, `RegisterForActiveControllerChanges`, `RegisterForConfigSelectionChanges`, `RegisterForControllerAccountChanges`, `RegisterForControllerAnalogInputMessages`, `RegisterForControllerCommandMessages`, `RegisterForControllerConfigCloudStateChanges`, `RegisterForControllerConfigInfoMessages`, `RegisterForControllerInputMessages`, `RegisterForDualSenseUpdateNotification`, `RegisterForGameKeyboardMessages`, `RegisterForKeyboardDeviceChanges`, `RegisterForRemotePlayConfigChanges`, `RegisterForShowControllerLayoutPreviewMessages`, `RegisterForTouchMenuInputMessages`, `RegisterForTouchMenuMessages`, `RegisterForUIVisualization`, `RegisterForUnboundControllerListChanges`, `RegisterForUserDismissKeyboardMessages`, `RegisterForUserKeyboardMessages`, `RequestGyroActive`, `RequestRemotePlayControllerConfigs`, `ResetControllerBindings`, `ResolveCloudedControllerConfigConflict`, `RestoreControllerPersonalizationSettings`, `SaveControllerCalibration`, `SaveControllerPersonalizationSettings`, `SaveControllerSounds`, `SaveEditingControllerConfiguration`, `SetControllerConfigurationModeShiftBinding`, `SetControllerHapticSetting`, `SetControllerMappingString`, `SetControllerName`, `SetControllerNintendoLayoutSetting`, `SetControllerPersonalizationName`, `SetControllerPersonalizationSetting`, `SetControllerPersonalizationSettingFloat`, `SetControllerRumbleSetting`, `SetControllerUseUniversalFaceButtonGlyphs`, `SetCursorActionset`, `SetDualSenseUpdateNotification`, `SetEditingControllerConfigurationActionSet`, `SetEditingControllerConfigurationInputActivator`, `SetEditingControllerConfigurationInputActivatorEnabled`, `SetEditingControllerConfigurationInputBinding`, `SetEditingControllerConfigurationMiscSetting`, `SetEditingControllerConfigurationSourceMode`, `SetEditingTritonCapSenseSettings`, `SetGamepadKeyboardText`, `SetKeyboardActionset`, `SetMousePosition`, `SetSelectedConfigForApp`, `SetSteamControllerDonglePairingMode`, `SetVirtualMenuKeySelected`, `SetWebBrowserActionset`, `SetXboxDriverInstallState`, `ShowControllerSettings`, `StandaloneKeyboardDismissed`, `StartControllerDeviceSupportFlow`, `StartEditingControllerConfigurationForAppIDAndControllerIndex`, `StartUIVisualization`, `StopEditingControllerConfiguration`, `StopUIVisualization`, `SwapControllerConfigurationSourceModes`, `SwapControllerModeInputBindings`, `SwapControllerOrder`, `SyncCloudedControllerConfigs`, `TriggerHapticPulse`, `TriggerSimpleHapticEvent`, `TurnOffController`, `UploadChangesForCloudedControllerConfigs`


### `SteamClient.InstallFolder`

`AddInstallFolder`, `BrowseFilesInFolder`, `CancelMove`, `GetInstallFolders`, `GetPotentialFolders`, `MoveInstallFolderForApp`, `RefreshFolders`, `RegisterForInstallFolderChanges`, `RegisterForMoveContentProgress`, `RegisterForRepairFolderFinished`, `RemoveInstallFolder`, `RepairInstallFolder`, `SetDefaultInstallFolder`, `SetFolderLabel`


### `SteamClient.Installs`

`CancelInstall`, `ContinueInstall`, `GetInstallManagerInfo`, `OpenInstallBackup`, `OpenInstallWizard`, `OpenUninstallWizard`, `RegisterForShowConfirmUninstall`, `RegisterForShowFailedUninstall`, `RegisterForShowInstallWizard`, `RegisterForShowRegisterCDKey`, `SetAppList`, `SetCreateShortcuts`, `SetInstallFolder`


### `SteamClient.MachineStorage`

`DeleteKey`, `GetJSON`, `GetString`, `SetObject`, `SetString`


### `SteamClient.Messaging`

`PostMessage`, `RegisterForMessages`


### `SteamClient.Music`

`DecreaseVolume`, `IncreaseVolume`, `PlayNext`, `PlayPrevious`, `RegisterForMusicPlaybackChanges`, `RegisterForMusicPlaybackPosition`, `SetPlaybackPosition`, `SetPlayingRepeatStatus`, `SetPlayingShuffled`, `SetVolume`, `ToggleMuteVolume`, `TogglePlayPause`


### `SteamClient.Notifications`

`RegisterForNotifications`


### `SteamClient.OpenVR`

`ExtendActivityTimeout`, `GetMutualCapabilities`, `GetWebSecret`, `InstallVR`, `QuitAllVR`, `RegisterForButtonPress`, `RegisterForHMDActivityLevelChanged`, `RegisterForInstallDialog`, `RegisterForStartupErrors`, `RegisterForVRHardwareDetected`, `RegisterForVRModeChange`, `RegisterForVRSceneAppChange`, `RegisterForVRTrackedDevices`, `SetOverlayInteractionAffordance`, `StartVR`, `TriggerOverlayHapticEffect`

- `Device.` (nested): `BIsConnected`, `RegisterForDeviceConnectivityChange`, `RegisterForVRDeviceSeenRecently`

- `DeviceProperties.` (nested): `GetBoolDeviceProperty`, `GetDoubleDeviceProperty`, `GetFloatDeviceProperty`, `GetInt32DeviceProperty`, `GetStringDeviceProperty`, `RegisterForDevicePropertyChange`

- `Keyboard.` (nested): `Hide`, `RegisterForStatus`, `SendDone`, `SendText`, `Show`

- `PathProperties.` (nested): `GetBoolPathProperty`, `GetDoublePathProperty`, `GetFloatPathProperty`, `GetInt32PathProperty`, `GetStringPathProperty`, `RegisterForPathPropertyChange`, `SetBoolPathProperty`, `SetDoublePathProperty`, `SetFloatPathProperty`, `SetInt32PathProperty`, `SetStringPathProperty`

- `VRNotifications.` (nested): `HideCustomNotification`, `RegisterForNotificationEvent`, `ShowCustomNotification`

- `VROverlay.` (nested): `HideDashboard`, `IsDashboardVisible`, `RegisterForButtonPress`, `RegisterForCursorMovement`, `RegisterForOverlayMousePressEvents`, `RegisterForThumbnailChanged`, `RegisterForVisibilityChanged`, `ShowDashboard`, `SwitchToDashboardOverlay`


### `SteamClient.Overlay`

`DestroyGamePadUIDesktopConfiguratorWindow`, `GetOverlayBrowserInfo`, `HandleGameWebCallback`, `HandleProtocolForOverlayBrowser`, `RegisterForActivateOverlayRequests`, `RegisterForMicroTxnAuth`, `RegisterForMicroTxnAuthDismiss`, `RegisterForNotificationPositionChanged`, `RegisterForOverlayActivated`, `RegisterForOverlayBrowserProtocols`, `RegisterOverlayBrowserInfoChanged`, `SetOverlayState`


### `SteamClient.Parental`

`LockParentalLock`, `RegisterForParentalPlaytimeWarnings`, `RegisterForParentalSettingsChanges`, `UnlockParentalLock`


### `SteamClient.RemotePlay`

`BCanAcceptInviteForGame`, `BCanCreateInviteForGame`, `BRemotePlayTogetherGuestOnPhoneOrTablet`, `CancelInviteAndSession`, `CancelRemoteClientPairing`, `CloseGroup`, `CreateInviteAndSession`, `EnableWifiRadioSoftwareState`, `GetClientID`, `GetClientStreamingBitrate`, `GetClientStreamingQuality`, `GetControllerType`, `GetGameSystemVolume`, `GetPerUserInputSettings`, `GetPerUserInputSettingsWithGuestID`, `GetRemotePlayTogetherGroupIDForOverlayPID`, `HasRemoteDevicePIN`, `IdentifyController`, `InstallAudioDriver`, `InstallInputDriver`, `MoveControllerToSlot`, `PairViaWifiAP`, `RegisterForAdditionalParentalBlocks`, `RegisterForAudioDriverPrompt`, `RegisterForBitrateOverride`, `RegisterForControllersUpdated`, `RegisterForDevicesChanges`, `RegisterForGroupCreated`, `RegisterForGroupDisbanded`, `RegisterForInputDriverPrompt`, `RegisterForInputDriverRestartNotice`, `RegisterForInputUsed`, `RegisterForInviteResult`, `RegisterForNetworkUtilizationUpdate`, `RegisterForPlaceholderStateChanged`, `RegisterForPlayerInputSettingsChanged`, `RegisterForQualityOverride`, `RegisterForRemoteClientLaunchFailed`, `RegisterForRemoteClientStarted`, `RegisterForRemoteClientStopped`, `RegisterForRemoteDeviceAuthorizationCancelled`, `RegisterForRemoteDeviceAuthorizationRequested`, `RegisterForRemoteDevicePairingPINChanged`, `RegisterForRemoteDeviceSpectatePermissionCanceled`, `RegisterForRemoteDeviceSpectatePermissionRequested`, `RegisterForRestrictedSessionChanges`, `RegisterForSessionJoined`, `RegisterForSessionStarted`, `RegisterForSessionStopped`, `RegisterForSettingsChanges`, `RegisterForVRStreamingInvitation`, `SetClientStreamingBitrate`, `SetClientStreamingQuality`, `SetGameSystemVolume`, `SetPerUserControllerInputEnabled`, `SetPerUserControllerInputEnabledWithGuestID`, `SetPerUserKeyboardInputEnabled`, `SetPerUserKeyboardInputEnabledWithGuestID`, `SetPerUserMouseInputEnabled`, `SetPerUserMouseInputEnabledWithGuestID`, `SetRemoteDeviceAuthorized`, `SetRemoteDevicePIN`, `SetRemoteDeviceSpectateAllowed`, `SetRemotePlayEnabled`, `SetStreamingClientConfig`, `SetStreamingClientConfigEnabled`, `SetStreamingDesktopToRemotePlayTogetherEnabled`, `SetStreamingP2PScope`, `SetStreamingServerConfig`, `SetStreamingServerConfigEnabled`, `StartDesktopStream`, `StopRemoteClientStream`, `StopStreamingSession`, `StopStreamingSessionAndSuspendDevice`, `UnlockH264`, `UnpairLocalWifiAP`, `UnpairRemoteClient`, `UnpairRemoteDevice`, `UnpairRemoteDevices`


### `SteamClient.RoamingStorage`

`DeleteKey`, `GetJSON`, `GetString`, `SetObject`, `SetString`


### `SteamClient.Screenshots`

`DeleteLocalScreenshot`, `DeleteLocalScreenshots`, `GetAllAppsLocalScreenshots`, `GetAllAppsLocalScreenshotsCount`, `GetAllAppsLocalScreenshotsRange`, `GetAllLocalScreenshots`, `GetGameWithLocalScreenshots`, `GetLastScreenshotTaken`, `GetLocalScreenshotByHandle`, `GetLocalScreenshotCount`, `GetLocalScreenshotPath`, `GetNumGamesWithLocalScreenshots`, `GetTotalDiskSpaceUsage`, `ShowScreenshotInSystemViewer`, `ShowScreenshotsOnDisk`, `UploadLocalScreenshot`


### `SteamClient.ServerBrowser`

`AddFavoriteServer`, `AddFavoriteServersByIP`, `CancelServerQuery`, `ConnectToServer`, `CreateFriendGameInfoDialog`, `CreateServerGameInfoDialog`, `CreateServerListRequest`, `DestroyGameInfoDialog`, `DestroyServerListRequest`, `GetMultiplayerGames`, `GetServerListPreferences`, `PingServer`, `RegisterForFavorites`, `RegisterForFriendGamePlayed`, `RegisterForGameInfoDialogs`, `RegisterForPlayerDetails`, `RegisterForServerFriends`, `RegisterForServerInfo`, `RemoveFavoriteServer`, `RemoveHistoryServer`, `RequestPlayerDetails`, `RequestServerFriends`, `SetServerListPreferences`


### `SteamClient.Settings`

`AddClientBeta`, `ClearAllHTTPCaches`, `ClearDownloadCache`, `GetAccountSettings`, `GetAppUsesP2PVoice`, `GetAvailableLanguages`, `GetAvailableTimeZones`, `GetCurrentLanguage`, `GetGlobalCompatTools`, `GetMonitorInfo`, `GetRegisteredSteamDeck`, `GetTimeZone`, `GetWindowed`, `IgnoreSteamDeckRewards`, `OpenWindowsMicSettings`, `RegisterForAppsWithAutoUpdateOverrides`, `RegisterForMicVolumeUpdates`, `RegisterForSettingsArrayChanges`, `RegisterForSettingsChanges`, `RegisterForTimeZoneChange`, `ReinitMicSettings`, `RenderHotkey`, `SelectClientBeta`, `SetCurrentLanguage`, `SetEnableSoftProcessKill`, `SetHostname`, `SetMicTestMode`, `SetPreferredMonitor`, `SetRegisteredSteamDeck`, `SetSaveAccountCredentials`, `SetSetting`, `SetTimeZone`, `SetUseNintendoButtonLayout`, `SetUseUniversalFaceButtonGlyphs`, `SetWindowed`, `SpecifyGlobalCompatTool`, `ToggleSteamInstall`


### `SteamClient.SharedConnection`

`AllocateSharedConnection`, `Close`, `RegisterOnBinaryMessageReceived`, `RegisterOnLogonInfoChanged`, `RegisterOnMessageReceived`, `SendMsg`, `SendMsgAndAwaitBinaryResponse`, `SendMsgAndAwaitResponse`, `SubscribeToClientServiceMethod`, `SubscribeToEMsg`


### `SteamClient.Stats`

`RecordActivationEvent`, `RecordDisplayEvent`


### `SteamClient.SteamChina`

`GetCustomLauncherAppID`


### `SteamClient.Storage`

`DeleteKey`, `GetJSON`, `GetString`, `SetObject`, `SetString`


### `SteamClient.Streaming`

`AcceptStreamingEULA`, `CancelStreamGame`, `RegisterForStreamingClientFinished`, `RegisterForStreamingClientLaunchProgress`, `RegisterForStreamingClientStarted`, `RegisterForStreamingLaunchComplete`, `RegisterForStreamingPrelaunchCheck`, `RegisterForStreamingShowEula`, `RegisterForStreamingShowLaunchOptions`, `RegisterForStreamingStillDownloading`, `StreamingContinueStreamGame`, `StreamingSetLaunchOption`


### `SteamClient.System`

`CopyFile`, `CopyFilesToClipboard`, `CreateTempPath`, `ExitFakeCaptivePortal`, `FormatStorage`, `GetOSType`, `GetSystemInfo`, `IsDeckFactoryImage`, `IsSteamInTournamentMode`, `MoveFile`, `NotifyGameOverlayStateChanged`, `OpenFileDialog`, `OpenInSystemBrowser`, `OpenLocalDirectoryInSystemExplorer`, `RebootToAlternateSystemPartition`, `RegisterForAirplaneModeChanges`, `RegisterForBatteryStateChanges`, `RegisterForFormatStorageProgress`, `RegisterForSettingsChanges`, `RestartPC`, `SetAirplaneMode`, `ShutdownPC`, `SteamRuntimeSystemInfo`, `SuspendPC`, `UpdateSettings`, `VideoRecordingDriverCheck`

- `Audio.` (nested): `ClearDefaultDeviceOverride`, `GetApps`, `GetDevices`, `RegisterForAppAdded`, `RegisterForAppRemoved`, `RegisterForAppVolumeChanged`, `RegisterForDeviceAdded`, `RegisterForDeviceRemoved`, `RegisterForDeviceVolumeChanged`, `RegisterForServiceConnectionStateChanges`, `RegisterForVolumeButtonPressed`, `SetAppVolume`, `SetDefaultDeviceOverride`, `SetDeviceVolume`

- `Devkit.` (nested): `RegisterForPairingPrompt`, `RespondToPairingPrompt`, `SetPairing`

- `Display.` (nested): `EnableUnderscan`, `RegisterForBrightnessChanges`, `SetBrightness`, `SetUnderscanLevel`

- `Network.` (nested): `ForceRefresh`, `ForceTestConnectivity`, `GetProxyInfo`, `RegisterForAppSummaryUpdate`, `RegisterForConnectionStateUpdate`, `RegisterForConnectivityTestChanges`, `RegisterForDeviceChanges`, `SetFakeLocalSystemState`, `SetProxyInfo`, `SetWifiEnabled`, `StartScanningForNetworks`, `StopScanningForNetworks`

- `Report.` (nested): `GenerateSystemReport`, `SaveToDesktop`, `Submit`

- `UI.` (nested): `CloseGameWindow`, `GetGameWindowsInfo`, `RegisterForFocusChangeEvents`, `RegisterForOverlayGameWindowFocusChanged`, `RegisterForSystemKeyEvents`


### `SteamClient.UI`

`EnsureMainWindowCreated`, `ExitBigPictureMode`, `GetDesiredSteamUIWindows`, `GetOSEndOfLifeInfo`, `GetUIMode`, `NotifyAppInitialized`, `RegisterDesiredSteamUIWindowsChanged`, `RegisterForClientConVar`, `RegisterForErrorCondition`, `RegisterForKioskModeResetSignal`, `RegisterForStartupFinished`, `RegisterForUIModeChanged`, `RegisterMoveGamepadUIMainWindowToPrimaryDisplay`, `ResetErrorCondition`, `SetUIMode`


### `SteamClient.Updates`

`ApplyUpdates`, `CheckForUpdates`, `GetCurrentOSBranch`, `GetOSBranchList`, `RegisterForUpdateStateChanges`, `SelectOSBranch`


### `SteamClient.URL`

`ExecuteSteamURL`, `GetSteamURLList`, `GetWebSessionID`, `RegisterForRunSteamURL`, `RegisterForSteamURLChanges`


### `SteamClient.User`

`AuthorizeMicrotxn`, `CancelLogin`, `CancelMicrotxn`, `CancelRefreshLogin`, `CancelShutdown`, `ChangeUser`, `Connect`, `FlipToLogin`, `ForceShutdown`, `ForgetPassword`, `GetIPCountry`, `GetLoginProgress`, `GetLoginUsers`, `GoOffline`, `GoOnline`, `OnCloseSaveHardwareDialog`, `OptOutOfSurvey`, `PrepareForSystemSuspend`, `Reconnect`, `RegisterForConnectionAttemptsThrottled`, `RegisterForCurrentUserChanges`, `RegisterForLoginStateChange`, `RegisterForLoginUsersChanged`, `RegisterForPrepareForSystemSuspendProgress`, `RegisterForResumeSuspendedGamesProgress`, `RegisterForShowHardwareSurvey`, `RegisterForShutdownDone`, `RegisterForShutdownFailed`, `RegisterForShutdownStart`, `RegisterForShutdownState`, `RegisterShowSaveHardwareDialog`, `RemoveAllUsers`, `RemoveUser`, `RequestSupportSystemReport`, `ResumeSuspendedGames`, `RunSurvey`, `SendSurvey`, `SetAsyncNotificationEnabled`, `SetCheckForUpdatesOnRestart`, `SetLoginCredentials`, `ShouldShowUserChooser`, `ShowSaveHardwareDialog`, `SignOutAndRestart`, `StartLogin`, `StartOffline`, `StartRefreshLogin`, `StartRestart`, `StartShutdown`


### `SteamClient.WebChat`

`BSuppressPopupsInRestore`, `GetCurrentUserAccountID`, `GetLocalAvatarBase64`, `GetLocalPersonaName`, `GetOverlayChatBrowserInfo`, `GetPrivateConnectString`, `GetPushToTalkEnabled`, `GetSignIntoFriendsOnStart`, `GetUIMode`, `OnGroupChatUserStateChange`, `OnNewGroupChatMsgAdded`, `OpenURLInClient`, `RegisterForComputerActiveStateChange`, `RegisterForFriendPostMessage`, `RegisterForMouseXButtonDown`, `RegisterForPushToTalkStateChange`, `RegisterForUIModeChange`, `RegisterOverlayChatBrowserInfoChanged`, `SetActiveClanChatIDs`, `SetNumChatsWithUnreadPriorityMessages`, `SetPersonaName`, `SetPushToMuteEnabled`, `SetPushToTalkEnabled`, `SetPushToTalkHotKey`, `SetPushToTalkMouseButton`, `SetVoiceChatActive`, `SetVoiceChatStatus`, `ShowChatRoomGroupDialog`, `ShowFriendChatDialog`, `UnregisterForMouseXButtonDown`


### `SteamClient.WebUITransport`

`GetTransportInfo`, `NotifyTransportFailure`


### `SteamClient.Window`

`BringToFront`, `Close`, `DefaultMonitorHasFullscreenWindow`, `GetDefaultMonitorDimensions`, `GetMousePositionDetails`, `GetWindowDetails`, `GetWindowDimensions`, `GetWindowRestoreDetails`, `HideWindow`, `IsWindowMaximized`, `IsWindowMinimized`, `MarkLastFocused`, `Minimize`, `MoveTo`, `MoveToLocation`, `PositionWindowRelative`, `ProcessShuttingDown`, `ResizeTo`, `RestoreWindowSizeAndPosition`, `SetComposition`, `SetGamepadUIAutoDisplayScale`, `SetGamepadUIManualDisplayScaleFactor`, `SetHideOnClose`, `SetKeyFocus`, `SetMaxSize`, `SetMinSize`, `SetModal`, `SetResizeGrip`, `SetWindowFlashing`, `SetWindowIcon`, `ShowWindow`, `ToggleFullScreen`, `ToggleMaximize`


---

## Useful non-`SteamClient` globals

### `window.settingsStore.settings` — 26 keys

Includes the compat trio that mirrors `CCompatManager` state: **`bCompatEnabled`**,
**`bCompatEnabledForOtherTitles`**, **`strCompatTool`** (all read-only reflections;
on macOS they stay `false`/`false`/`""` because `m_bCompatEnabled` latches on the
host oslist — see [#16](https://github.com/Superd22/macos-steam/issues/16)).

`bChangeBetaEnabled`, `bCompatEnabled`, `bCompatEnabledForOtherTitles`, `bDisplayIsExternal`, `bDisplayIsUsingAutoScale`, `bEnableSoftProcessKill`, `bIsInClientBeta`, `bIsInDesktopUIBeta`, `bIsSteamSideload`, `bIsValveEmail`, `bUnderscanEnabled`, `eClientBetaState`, `flAutoDisplayScaleFactor`, `flCurrentDisplayScaleFactor`, `flCurrentUnderscanLevel`, `flMaxDisplayScaleFactor`, `flMinDisplayScaleFactor`, `nAvailableBetas`, `nSelectedBetaID`, `strCompatTool`, `strDisplayName`, `strSelectedBetaName`, `vecAvailableClientBetas`, `vecNightModeScheduledHours`, `vecValidAutoUpdateRestrictHours`, `vecValidDownloadRegions`

### `window.appStore` — prototype methods
`BIsAppPrivate`, `CompareSortAs`, `GetAlbumCoverURLForApp`, `GetAppOverviewByAppID`, `GetAppOverviewByGameID`, `GetCachedAlbumCoverURL`, `GetCachedVerticalCapsuleURL`, `GetCustomHeroImageURLs`, `GetCustomImageURLs`, `GetCustomLandcapeImageURLs`, `GetCustomLogoImageURLs`, `GetCustomSortAs`, `GetCustomVerticalCapsuleURLs`, `GetIconURLForApp`, `GetLocalizationForStoreTag`, `GetPregeneratedVerticalCapsuleForApp`, `GetStorePageURLForApp`, `GetTopStoreTags`, `GetVerticalCapsuleURLForApp`, `HandleSteamVRAppIconRequest`, `Init`, `OnCloudStorageChanged`, `OnPrivateAppsChanged`, `RefreshTagsIfNeeded`, `SetCustomSortAs`, `UpdateAppOverview`, `UpdatePrivateApps`

### `window.appDetailsStore` — prototype methods
`AppDetailsChanged`, `BAchievementIsHiddenAndAchieved`, `BHasMarketPresence`, `BHasRecentlyLaunched`, `BIsWorkshopVisible`, `CMInterface`, `ClearCustomLogoPosition`, `GetAchievements`, `GetAjaxLibraryAppDetails`, `GetAppData`, `GetAppDetails`, `GetAppDetailsSpotlight`, `GetAssociations`, `GetCustomLogoPosition`, `GetDescriptions`, `GetHeaderImages`, `GetHeaderImagesForAppId`, `GetHeroBlurImages`, `GetHeroBlurImagesForAppId`, `GetHeroImages`, `GetHeroImagesForAppId`, `GetLogoImages`, `GetLogoImagesForAppId`, `Init`, `MarkAppAsRecentlyLaunched`, `RegisterForAppData`, `RequestAchievements`, `RequestAppDetails`, `RequestAppDetailsSpotlight`, `RequestAssociationData`, `RequestCustomImageInfo`, `RequestDescriptionsData`, `SaveCustomLogoPosition`, `SetAjaxLibraryAppDetails`, `UnregisterForAppData`, `ValidateCustomImageInfo`

### `window.appAchievementProgressCache` — prototype methods
`BGameHasAchievements`, `GetAchievementProgress`, `Init`, `LoadCacheFile`, `OnAchievementNotification`, `QueueCacheUpdate`, `RequestCacheUpdate`, `SaveCacheFile`

`appStore.GetAppOverviewByAppID(appid)` returns the library overview object whose
fields include `display_status`, `installed`, `size_on_disk`, `per_client_data` —
the object #15 and #16 read their verdicts from.

Other objects present: `appDetailsStore`, `securitystore`, `uiStore`, `loginStore`,
`MainWindowBrowserManager`, `appAchievementProgressCache`.

## Raw capture

The mechanical JSON dumps behind this document (namespace tree with types, prototype
methods, globals) were captured with the ~30-line `cdp.mjs` harness described above;
regenerate at any time with the three-step recipe.
