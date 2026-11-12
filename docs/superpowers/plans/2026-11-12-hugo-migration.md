# Hugo + Stack migration implementation plan

Goal: Migrate the original eight posts and assets to Hugo 0.169.0 Extended + Stack 4.0.3.
Architecture: Complete local source project, vendored theme, split TOML configuration, native GitHub Pages deployment.
Authorization: User requested direct implementation using the original Stack theme.

- [x] Inspect original Markdown, published pages and the pinned upstream theme.
- [x] Preserve post bodies, slugs, dates, local images and avatar; restore utility pages and old about aliases.
- [x] Replace obsolete pagination, locale, avatar, widget and image settings with current equivalents.
- [x] Build with pinned Hugo, verify routes, local assets, search index and original content retention.
- [x] Add a GitHub Actions build/deploy workflow and maintenance documentation; verify workflow against upstream actions.
- [x] Package the project. Desktop/mobile visual verification blocked by browser access denial; no screenshot or visual claim.

Verification ledger:
- All 10 original source files match their SHA-256 values.
- All 117 legacy HTML/XML/image URLs exist.
- Production build succeeds with --panicOnWarning; Stack Sass import/global-builtin deprecations are explicitly documented and scoped.
- Reproduced future-post CI failure, changed search check to subset containment, verified new posts pass.
- External Waline GET /comment returns 500 (archived LeanCloud application); preserved endpoint with comments disabled.
- Original Markdown external image host cannot resolve; preserved original reference without build-time fetch.
- Independent review completed; no further material findings after search-check repair.

Review focus: Chinese paths; search JSON; code and inline HTML; legacy redirects and image URLs; failed external services.
Configuration/content migration uses real Hugo builds and generated-output checks rather than mirrored unit tests.
