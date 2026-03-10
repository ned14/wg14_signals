# Agentic coding guidelines

1. All source and header files MUST be kept compatible with the 2011 ISO
C standard. Do NOT use C++ at any time.
2. Run `clang-format` on every changed header and source file. Do NOT run
`clang-format` on cmake files.
3. When building and testing, extract what to do for the current platform
from `.github/workflows/ci.yml`.
