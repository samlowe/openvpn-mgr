# Core Development Guidelines

- Keep changes focused to a few areas at a time. Don't make sweeping changes unrelated to the task
- Run tests often, especially after completing work or adding tests. Use `make test` (builds and runs `./test_core`)
- Build with `make`; use `make clean` to remove build artifacts
- Make one change at a time for complex tasks, verify it works before proceeding
- If a new dependency/library is required, ask the user first (with why current dependencies are insufficient)
- Use ripgrep (rg) instead of grep when available. But remember rg recurses by default and `-r` is replace (unlike grep)

# Important Don'ts

- Don't delete any files/folders without asking the user first
- Don't remove existing comments or commented-out code unless explicitly asked
- Never run any git write/commit/update commands without asking first
- Never run destructive operations (e.g. dropping tables) without asking first
- Do not add redundant/pointless inline comments; use meaningful names so the need for comments is minimised
- Use relative paths (from the project root) rather than absolute paths in commands. Avoid cd'ing into subfolders, but if you have to, then always `cd ..` out. No `pwd`

# Code Style and Approach

- C11 with `-Wall -Wextra -Wpedantic`; match existing style in `src/` and `tests/`
- Do not add comments that are redundant or obvious from the code. Write self-documenting code, well named vars/functions and sub-functions for encapsulation. Remember SRP and DRY!
- Add brief comments only where behaviour is non-obvious

# Communication

- If you notice a problem or see a better way, discuss with user before proceeding
- If a test is failing and it might be a bug in the app code ask user before proceeding. But if a test just needs updating to align with a code change, go ahead and update it
- If getting stuck in a rabbithole, stop, review, and give options to user
