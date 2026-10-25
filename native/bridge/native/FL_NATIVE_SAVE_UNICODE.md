# Native FL project path encoding

The supported native build can allocate FPC UnicodeString paths with code page0 or1200 and two-byte UTF16 elements. The project Save guard accepts those two code-page values only when the element width is exactly2. All other header, length, reference, path, repeat-read, caller-chain, temporary-file and destructive-commit guards remain unchanged.

This is a native path-reader compatibility correction. It does not relax the requirement that all live ARA component archives be frozen and serialized before a project commit.
