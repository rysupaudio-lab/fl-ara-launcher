# Normal native FL project argument — 2026-10-24

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


Static acceptance route is established for the exact current FL26 engine.
No app has been launched by this investigation. Parsing a path does not prove
that its project has loaded successfully; actual native model/file readback
remains mandatory in root's owned normal-app runtime test.

Use one absolute owned copied FLP path as a single argument:

```python
subprocess.Popen([str(private_osxfl), str(owned_project_flp)], env=private_env)
```

This directly starts the private copy. No LaunchServices process adoption is
needed. The launcher agent owns backup/preservation, private preferences,
process locks, file/payload pins and all prelaunch safety admission.

Current exact static route (engine ARM64 seal
`24b368d08cc861a1bf06ee8883233aa9ca579746825481bfe89f94e8e20eff2c`):

Do not add `--args` (that belongs to macOS `open`, not OsxFL), export switches,
or an extra `--` sentinel. A special end-of-options sentinel is not established
by this source route; `--` begins with FL's configured switch prefix.

The small OsxFL bootstrap only handles its special sole `-setenginepath`
case before creating the normal engine. The actual project parsing above lives
inside that engine. Official export documentation independently describes
macOS -R/-E/-F batch exports, but it was not used as proof of this normal
project argument route:
https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/fformats_save_export.htm

The installed Info.plist declares `.flp` as an Editor document type and has no
LSMultipleInstancesProhibited declaration. This does not qualify dispatching an
open event by shared bundle ID: that could reach Jordan's original instance.
Any future post-launch open API must target the exact owned process and prove
native loaded-path/model readback; it is separate from this direct startup route.
