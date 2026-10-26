# Contributing

Help people use ARA in more workflows: report a plug-in that worked, describe a bug, improve installation, or contribute a native host adapter.

- [Plug-in compatibility report](https://github.com/rysupaudio-lab/fl-ara-launcher/issues/new?template=plugin-compatibility.yml)
- [Another DAW / host proposal](https://github.com/rysupaudio-lab/fl-ara-launcher/issues/new?template=host-request.yml)
- [Porting and source map](docs/PORTING.md)

A useful report names your DAW/build, OS, plug-in/version and the edit or operation you tried. Say what worked or what went wrong; a full test suite is not required. Attach a small relevant log excerpt if helpful, with project paths, license data and other private information removed. Keep customer/user projects private.

For code, reuse the existing host and headless/native export tools. Choose checks that answer the changed behavior. Documentation changes do not require DAW launches, and each fix does not need a broad compatibility matrix. Preserve installed DAWs and active user sessions; use owned copies/profiles.

Describe the problem, the new behavior and the focused evidence in a pull request. Keep feature claims clear about whether they are demonstrated here or reported by the community. Do not bundle third-party applications, plug-ins, models or licenses.
