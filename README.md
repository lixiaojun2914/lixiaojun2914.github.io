# My Blog — Hugo + Stack

完整的博客源码项目，迁移自 lixiaojun2914/blog 和 lixiaojun2914/lixiaojun2914.github.io。

## 版本与本地预览

- Hugo **0.169.0 Extended**（`.hugo-version` 与 Actions 同步）
- Stack **4.0.3**（已随项目保存，无需拉取子模块或使用 Go Modules）
- Dart Sass **1.108.2**（必须使用官方独立二进制，放到 PATH；不要用普通 npm sass 代替 embedded 协议服务）
- Python 3（仅用于迁移完整性检查）

从 [Hugo Releases](https://github.com/gohugoio/hugo/releases/tag/v0.169.0) 和 [Dart Sass Releases](https://github.com/sass/dart-sass/releases/tag/1.108.2) 下载对应系统版本。macOS 的 Hugo 安装包选 `hugo_extended_0.169.0_darwin-universal.pkg`，Sass 选 macos-arm64 或 macos-x64，解压后将整个 `dart-sass` 目录放到 PATH 中。

```bash
cd hugo-blog
hugo version
sass --version
hugo server --bind 127.0.0.1
```

访问 http://localhost:1313/。查看草稿加 `-D`。

```bash
hugo --gc --minify --panicOnWarning
python3 scripts/verify_site.py
python3 scripts/verify_comments.py
node scripts/verify_giscus_theme.cjs
```

校验脚本检查迁移及后续明确更新的文件快照哈希；以后主动修改历史文章时，需要同步更新 `docs/migration-manifest.json` 中对应的 SHA-256。新增文章不会受历史哈希限制。

## 目录与配置

```text
config/_default/           站点、主题参数、菜单、Markdown、相关文章配置
content/post/<文章>/      每篇文章一个 Page Bundle：index.md + 图片
content/page/             关于、归档、搜索、链接等独立页面
content/categories/       保留的分类说明页
assets/img/               头像（可由 Hugo 处理）
assets/scss/custom.scss   自定义字体与样式
layouts/                  独立于主题的兼容覆盖
static/                   保留旧站图片 URL，包括旧 Hugo 的缩略图
archetypes/post/          新文章模板
scripts/verify_site.py    构建与迁移完整性检查
.github/workflows/        固定版本的构建、验证和 Pages 发布
themes/hugo-theme-stack/ 完整的上游主题（含 LICENSE）
```

所有站点配置在 `config/_default/`。根目录 `hugo.toml` 仅作为入口标记，不要另加一份重复配置。站名保留为 `My Blog`，可在 `config/_default/hugo.toml` 中修改；副标题采用旧文中的“记录学习的技能和遇到的问题”。

### 写文章

```bash
hugo new content --kind post post/my-new-note
```

编辑 Front Matter 的标题、slug、分类、标签，并将 `draft` 改为 `false` 后发布。配图直接放在同一目录。文章 URL 为 `/p/:slug/`，发布后尽量不改 slug；确需修改时，为旧地址添加 `aliases`。

### GitHub Pages 部署

1. 将本项目内容作为现有 `lixiaojun2914.github.io` 仓库的新源码提交（建议先备份原分支或走 PR）。不要将项目再包进一层 `hugo-blog/`；`hugo.toml` 应在仓库根目录。
2. 仓库 **Settings → Pages → Build and deployment → Source** 选择 **GitHub Actions**。
3. 合并到 `main` 或手动运行 `Build and deploy Hugo`。Pull Request 只构建与验证，不发布。
4. 发布地址保持 `https://lixiaojun2914.github.io/`。无须 PAT，也不需要单独提交 `public/`。

`migration/**` 分支会运行构建与完整性检查；只有 `main` 分支会部署到线上。GitHub Pages 的 Source 必须设为 GitHub Actions，才能发布。

### 后续升级

先修改 `.hugo-version` 和工作流中的 Hugo 版本；下载目标版本 Stack release 并整体替换主题目录，保留它的许可证；同步 `THEME_VERSION`。同时更新工作流中的下载校验和。不要直接编辑主题内部文件，自定义放在根目录 `assets/` 和 `layouts/`。

当前 Stack 仍含 Sass `@import` 和旧全局函数：样式覆盖只对这两类已知弃用提示做定向静默，版本固定在 Dart Sass 1.x；并不代表已将上游 SCSS 重写为 Sass 3 模块系统。

迁移细节和外部服务问题见 [MIGRATION.md](MIGRATION.md)。

## 评论系统（Giscus）

评论存储在本仓库 GitHub Discussions 中，读者使用 GitHub 登录后发表评论。配置在 `config/_default/params.toml` 的 `[comments.giscus]`。

- 使用 `pathname` + 严格匹配关联文章，修改标题不影响讨论；请保持已发布文章路径稳定。
- 使用公告类型的讨论分类，首次留言或反应时由 Giscus 自动创建对应讨论。
- 简体中文、明暗主题切换、表情反应与懒加载已启用。
- 关于、归档、搜索与链接页仍不显示评论。
- Giscus 应用仅需安装在这个仓库；它需要 Discussions 写入权限以管理评论。
- 原 Waline/LeanCloud 数据没有被删除，但不会自动迁入 GitHub。恢复旧应用后应先导出备份，再单独评估历史评论迁移。

主题的 Giscus 消息监听通过 `layouts/_partials/comments/provider/giscus.html` 作兼容覆盖，修正旧模板依赖全局 `event` 的问题；不修改主题本体。`scripts/verify_giscus_theme.cjs` 仅用 Node 内置模块检查主题切换，不会发送测试留言。
