# iOS signing on UE 5.8 — the three traps (repo-only)

Moved out of `docs/buyer/setup.md` 2026-08-12 by owner call: none of this is
plugin-specific — it is generic Unreal-on-iOS — so buyers get "iOS is supported" and
this file keeps the session-cost knowledge for whoever builds the demo or the test
matrix next. Chain proven end-to-end 2026-08-12: iPhone 11 Pro, iOS 26.0.1, UE 5.8.1
Launcher, Xcode 26. Full evidence: `~/VaCuusSession-ios/LOG.md` on the Mac (700 lines),
bead VaCuus-4lp (closed).

1. **The signing settings live in a different ini section than every older UE guide
   says.** On 5.8 modern Xcode is the only build path
   (`AppleToolChain.cs:1216-1218` marks `-ModernXcode` obsolete), and it reads
   `[/Script/MacTargetPlatform.XcodeProjectSettings]`
   (`XcodeProject.cs:2064-2600`, defaults `BaseEngine.ini:3452-3457`) — **not**
   `[/Script/IOSRuntimeSettings.IOSRuntimeSettings]`, which only the legacy path
   consulted (`IOSExports.cs:28-61`). In the host project's `Config/DefaultEngine.ini`:

   ```ini
   [/Script/MacTargetPlatform.XcodeProjectSettings]
   bUseAutomaticCodeSigning=True
   CodeSigningTeam=XXXXXXXXXX
   CodeSigningPrefix=com.yourcompany
   BundleIdentifier=com.yourcompany.yourgame
   ```

   Automatic signing defaults ON while the team defaults EMPTY — which is why an
   unconfigured build fails at .app finalization with "requires a development team"
   (passing `-project=` disables dummy signing, `AppleToolChain.cs:1265`).

2. **The Team ID is the certificate's OU, not the value in brackets after the name.**
   `IDEProvisioningTeams` may never populate; the reliable route is
   `security find-certificate -a -c "Apple Development" -p | openssl x509 -noout
   -subject` — take the 10 characters after `OU=`. The parenthesised id in the CN is
   the *certificate* id, the obvious misread. Cross-check: `codesign -dv` prints
   `TeamIdentifier=`.

3. **Building over SSH: unlock the login keychain in the same shell that launches the
   build.** The unlock does not cross SSH sessions — codesign fails with
   `errSecInternalComponent` while `security find-identity` looks healthy.
   `security set-key-partition-list -l "<cert label>"` is a proven NO-OP here (the
   label does not match the private key); it only appears to work because `-k` unlocks
   the keychain for its own session as a side effect.

Minted profiles land in `~/Library/Developer/Xcode/UserData/Provisioning Profiles/`,
not `~/Library/MobileDevice/Provisioning Profiles/` (older guides name the latter).
UBT already passes `-allowProvisioningUpdates`, so a registered, trusted device is
enough for Xcode to mint the free profile during the build.

**Unmeasured:** the free-personal-team path. The proven chain used a paid Apple
Developer account (365-day profile, 35 devices); free teams get 7-day profiles and a
3-device cap, so expect weekly re-signing — behaviour identical in principle, not
verified here.

Ad-hoc signing (`CODE_SIGN_IDENTITY=-`) is **refused** on iphoneos — there is no
signed-but-account-free middle ground; `CODE_SIGNING_ALLOWED=NO` assembles an unsigned
`.app` and real signing needs team + profile, nothing in between.
