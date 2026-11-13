---
title: "Hugo Blog搭建过程"
date: "2022-01-28"
lastmod: "2026-11-12"
description: "使用 Hugo 0.169.0、Stack 4.0.3、Dart Sass、GitHub Actions 与 Giscus 搭建和维护个人博客。"
slug: "test-post"
categories:
    - 博客建设
tags:
    - Hugo
    - Stack
    - GitHub Actions
    - Giscus
---

这篇文章最初写于 2022 年，当时使用的是 Hugo 0.92.0、Stack 3.7.0，以及 Waline + LeanCloud 评论。2026 年重新整理博客时，升级了 Hugo 和主题，补齐了完整源码、自动发布流程，并将评论切换到 Giscus。本文按这次实际搭建结果重写。

首次发布日期和原地址 `/p/test-post/` 保持不变，方便原来的链接和评论继续关联。

## 本次使用的版本与架构

| 组件 | 当前项目使用的版本或服务 | 用途 |
| --- | --- | --- |
| Hugo | 0.169.0 Extended | 生成静态站点 |
| Stack | 4.0.3 | 博客主题 |
| Dart Sass | 1.108.2 | 编译主题的 SCSS |
| GitHub Pages | GitHub Actions 发布 | 托管生成后的网页 |
| Giscus | GitHub Discussions | 保存评论与表情反应 |

这些是本次验证过并固定在项目里的版本，不表示以后始终是最新版本。升级时要同步修改本地工具和工作流中的版本，并重新检查构建结果。

现在的流程是：

```text
Markdown 文章、配置和主题源码
          ↓ git push
GitHub Actions：安装固定版本 → 构建 → 校验
          ↓
GitHub Pages：发布 public/ 中的静态文件
          ↓
浏览器打开文章 → Giscus 加载对应的 GitHub Discussion
```

Hugo 和 GitHub Pages 都不直接保存评论，Giscus 使用的是仓库 Discussions。

## 旧教程中哪些内容过期了

| 旧写法或旧做法 | 这次的处理 |
| --- | --- |
| 从主题的 `exampleSite/` 复制配置和示例文章 | 不再依赖旧目录；使用完整的站点源码与当前版本配置 |
| `paginate: 5` | 改用 `[pagination]` 下的 `pagerSize = 5` |
| `hasCJKLanguage: false` | 中文博客改为 `true` |
| 旧 `zh-cn` 主题语言配置 | 使用 `locale = "zh-CN"` 与 `defaultContentLanguage = "zh"`，匹配 Stack 4 的翻译文件 |
| 头像使用 `enabled/local/src` 对象 | 改成 `avatar = "img/avatar.png"` |
| `widgets.enabled` | 改成 `widgets.homepage` 和 `widgets.page` |
| 依赖默认 LibSass 编译 | 显式切换到 Dart Sass |
| Waline + LeanCloud | 换成 Giscus + GitHub Discussions |
| 手动生成并提交网页文件 | 提交源码，由 Actions 构建发布 |
| `hugo -D` 作为发布命令 | `-D` 只在需要预览草稿时使用，正式构建不包含草稿 |

Hugo 官方已经弃用内嵌 LibSass，Dart Sass 可用于不同的 Hugo 发行版。这里保留 Extended 版本，是为了与本项目已经验证的构建环境一致，并不是说使用 Dart Sass 必须安装 Extended。[Hugo Sass 文档](https://gohugo.io/functions/css/sass/)

## 安装本地工具

本地需要 Git、Hugo 和 Dart Sass。项目还提供了校验脚本：Python 使用 3.11 或更高版本，主题同步检查使用 Node.js，本次本地验证使用 Node.js 22。

为了复现本文，优先安装指定版本：

- [Hugo 0.169.0](https://github.com/gohugoio/hugo/releases/tag/v0.169.0)：选择系统对应的 Extended 发行包。macOS 可安装 `hugo_extended_0.169.0_darwin-universal.pkg`。
- [Dart Sass 1.108.2](https://github.com/sass/dart-sass/releases/tag/1.108.2)：Apple Silicon 使用 `macos-arm64`，Intel Mac 使用 `macos-x64`；其他系统选择对应发行包。

Dart Sass 解压后要保留完整的 `dart-sass` 目录，并把该目录加入 `PATH`。不要只移动启动脚本，也不要把普通的 `npm install -g sass` 当成这里使用的独立 Dart Sass 程序。

安装后检查：

```bash
hugo version
sass --version
python3 --version
node --version
```

前两项应分别显示 Hugo 0.169.0 和 Sass 1.108.2。Hugo 能否找到 Dart Sass，取决于它启动时的 `PATH`。[Hugo 安装说明](https://gohugo.io/installation/macos/)

## 获取完整源码

现在仓库中保存的是完整 Hugo 工程，文章、配置、主题和自动发布文件都在一起。复现本站，直接克隆即可：

```bash
git clone https://github.com/lixiaojun2914/lixiaojun2914.github.io.git
cd lixiaojun2914.github.io
hugo server --bind 127.0.0.1
```

打开 <http://localhost:1313/>。预览草稿时改用：

```bash
hugo server --bind 127.0.0.1 -D
```

这个仓库已经包含主题，无须再执行主题安装命令，也不依赖 Go Modules 或 npm 安装主题包。`.hugo-version` 是项目的版本记录；除非另有版本管理工具读取它，否则这个文件不会自动安装或切换 Hugo。

如果是从空目录新建另一个站点，可以先执行：

```bash
hugo new site my-blog
cd my-blog
curl --fail --location https://github.com/CaiJimmy/hugo-theme-stack/archive/refs/tags/v4.0.3.tar.gz --output stack.tar.gz
mkdir -p themes/hugo-theme-stack
tar -xzf stack.tar.gz -C themes/hugo-theme-stack --strip-components=1
```

然后配置站点与下文的兼容覆盖。这个命令只安装主题，不会自动得到本站的文章、校验脚本和发布工作流；这些需要从[本站源码](https://github.com/lixiaojun2914/lixiaojun2914.github.io)按需复制。不要再套用旧教程里的 `exampleSite/config.yaml` 路径。

## 目录如何组织

```text
.
├── hugo.toml                         # 入口标记，实际配置在 config/_default/
├── config/_default/
│   ├── hugo.toml                     # 地址、语言、分页、固定链接
│   ├── params.toml                   # Stack、侧栏、评论等参数
│   ├── menu.toml                     # 导航与社交链接
│   ├── markup.toml                   # Markdown、目录、公式、代码高亮
│   └── related.toml                  # 相关文章
├── content/
│   ├── post/<文章目录>/index.md       # 文章和配图放在同一个目录
│   ├── page/                         # 关于、归档、搜索、链接页
│   └── categories/                   # 分类说明
├── assets/
│   ├── img/avatar.png                # 头像
│   └── scss/custom.scss              # 自定义样式与中文字体
├── layouts/                          # 站点自己的模板覆盖
├── static/                           # 保留旧图片 URL 的静态文件
├── archetypes/post/index.md          # 新文章模板
├── themes/hugo-theme-stack/          # 固定版本的完整主题
├── scripts/                          # 构建后校验
└── .github/workflows/pages.yml       # 自动构建和发布
```

原来“一篇文章一个文件夹，正文与图片放一起”的结构是合理的，符合 Hugo 的 Page Bundle 方式，因此保留。主要调整是把它们放进 `content/post/`，并补齐站点配置和部署文件。

主题以完整源码的方式保存在仓库里，避免克隆时忘记拉取子模块。自定义内容放在站点根目录的 `assets/` 和 `layouts/`，不直接修改主题本体。更新主题时整体替换主题目录，并保留其许可证。

`public/`、`resources/` 等生成结果不提交到源码仓库。

## 站点基础配置

`config/_default/hugo.toml` 的实际配置如下：

```toml
baseURL = "https://lixiaojun2914.github.io/"
title = "My Blog"
locale = "zh-CN"
defaultContentLanguage = "zh"
hasCJKLanguage = true
timeZone = "Asia/Shanghai"
theme = "hugo-theme-stack"
enableEmoji = true
enableRobotsTXT = true

[pagination]
    pagerSize = 5

[permalinks.page]
    post = "/p/:slug/"
    page = "/:slug/"

[taxonomies]
    category = "categories"
    tag = "tags"
```

这里保留 `post = "/p/:slug/"`，所以旧文章仍使用原地址，包括 `/p/test-post/` 和中文路径。文章目录名可以便于本地管理，公开地址由 `slug` 决定。

根目录 `hugo.toml` 只作入口标记，不再另外保存一份重复的站点配置。Hugo 读取 `config/_default/` 下的配置文件，并按照文件名区分站点配置、主题参数等内容。

## Stack 侧栏、目录与中文显示

`config/_default/params.toml` 中，评论设置之外的部分如下：

```toml
mainSections = ["post"]
rssFullContent = true
favicon = "img/avatar.png"

[footer]
    since = 2020

[dateFormat]
    published = "2006-01-02"
    lastUpdated = "2006-01-02 15:04 MST"

[sidebar]
    emoji = "🍥"
    subtitle = "记录学习的技能和遇到的问题"
    avatar = "img/avatar.png"

[article]
    math = true
    toc = true
    readingTime = true
    headingAnchor = true

    [article.license]
        enabled = true
        default = "Licensed under [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)"

[widgets]
    homepage = [
        { type = "search" },
        { type = "archives", params = { limit = 5 } },
        { type = "categories", params = { limit = 10 } },
        { type = "tag-cloud", params = { limit = 15 } },
    ]
    page = [{ type = "toc" }]

[colorScheme]
    toggle = true
    default = "auto"
```

头像放在 `assets/img/avatar.png`。首页显示搜索、归档、分类和标签云，文章右侧显示目录。中文摘要和字数统计使用 `hasCJKLanguage = true`。

为了减少对外部字体服务的依赖，本站用 `layouts/_partials/head/custom-font.html` 覆盖主题默认字体加载，在 `assets/scss/custom.scss` 中选择中文系统字体：

```scss
:root {
    --base-font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", sans-serif;
}
```

导航在 [menu.toml](https://github.com/lixiaojun2914/lixiaojun2914.github.io/blob/main/config/_default/menu.toml) 中设置。首页、分类和标签是站点菜单；关于、归档、搜索等独立页面通过各自 Front Matter 加入菜单。

## 让主题使用 Dart Sass

Stack 4.0.3 的默认样式模板仍使用旧编译方式。本项目在 `layouts/_partials/head/style.html` 中覆盖它：

```go-html-template
{{- $sass := resources.Get "scss/style.scss" -}}
{{/* Stack 4.0.3 uses legacy Sass imports/functions; keep upstream intact.
     These two deprecations remain compatible with the pinned Dart Sass 1.x. */}}
{{- $opts := dict "transpiler" "dartsass" "outputStyle" "compressed"
    "silenceDeprecations" (slice "import" "global-builtin") -}}
{{- $style := $sass | css.Sass $opts | minify | fingerprint "sha256" -}}
<link rel="stylesheet" href="{{ $style.RelPermalink }}" integrity="{{ $style.Data.Integrity }}" crossorigin="anonymous">
```

这里明确指定 `transpiler = dartsass`。主题当前仍有 Sass `@import` 和全局函数的旧写法，因此只对 `import`、`global-builtin` 两类已知弃用提示做定向静默，并固定使用 Dart Sass 1.108.2。它们并没有因此被改写成新的 Sass 模块语法，未来升级 Sass 大版本时仍需要重新检查。

其他构建警告仍会被 `--panicOnWarning` 检出。

## Markdown、代码与公式

`config/_default/markup.toml` 如下：

```toml
[goldmark.renderer]
    # Original Markdown examples contain raw HTML such as <br>.
    unsafe = true

[goldmark.extensions.passthrough]
    enable = true
    [goldmark.extensions.passthrough.delimiters]
        block = [["\\[", "\\]"], ["$$", "$$"]]
        inline = [["\\(", "\\)"]]

[tableOfContents]
    startLevel = 2
    endLevel = 4
    ordered = true

[highlight]
    noClasses = false
    lineNos = true
    lineNumbersInTable = true
    tabWidth = 4
```

`unsafe = true` 是为了渲染已有 Markdown 示例中的 `<br>` 等 HTML，适用于本站自己维护的正文。公式透传与 Stack 的 KaTeX 配置配合使用；代码高亮、复制按钮和文章目录由 Hugo 与主题完成。

旧 Markdown 笔记中的一张外链图片已经无法解析。本站保留原文 URL，并让远程正文图片直接由浏览器请求，不在构建时下载；对应覆盖在 `layouts/_markup/render-image.html`。这样失效外链不会阻断整个博客的发布，但也不会凭空修复已经丢失的图片。

## 使用 Giscus 评论

这次用 Giscus 替换 Waline。选择它主要是因为技术博客的读者通常有 GitHub 账号，而且不必再单独维护评论数据库。

需要先完成两项 GitHub 设置：

1. 在仓库 **Settings → General → Features** 中启用 **Discussions**。
2. 安装[官方 Giscus 应用](https://github.com/apps/giscus)，选择 **Only select repositories**，仅授权博客仓库。

然后打开 [Giscus 配置页面](https://giscus.app/zh-CN)，选择仓库和 `Announcements` 公告分类，获取仓库与分类的 ID。本站配置在 `params.toml`：

```toml
[comments]
    enabled = true
    provider = "giscus"

    [comments.giscus]
        repo = "lixiaojun2914/lixiaojun2914.github.io"
        repoID = "R_kgDOGwEMGg"
        category = "Announcements"
        # Announcements category returned by the official Giscus API.
        categoryID = "DIC_kwDOGwEMGs4DK-ki"
        mapping = "pathname"
        strict = 1
        reactionsEnabled = 1
        emitMetadata = 0
        inputPosition = "top"
        lightTheme = "light"
        darkTheme = "dark_dimmed"
        lang = "zh-CN"
        loading = "lazy"
```

上面的仓库和分类 ID 属于本站，是公开标识，不是密钥。搭建自己的博客时必须换成自己的值。

`mapping = "pathname"` 按文章路径查找讨论，`strict = 1` 使用严格匹配，因此改文章标题不会改变评论关联；修改文章路径则要考虑原讨论如何继续关联。

首次有人留言或添加反应时，Giscus 才会为文章创建对应讨论，无须提前给每篇文章建一个 Discussion。读者需要登录 GitHub 并按提示授权后评论。[Giscus 官方说明](https://giscus.app/zh-CN)

本站还在 `layouts/_partials/comments/provider/giscus.html` 中修正了主题消息监听依赖全局 `event` 的问题，使其使用实际传入的事件对象，并过滤无关消息。浅色与深色主题切换有独立脚本校验。

关于、归档、搜索和链接页继续关闭评论。旧 Waline 入口已经移除，但 LeanCloud 中的历史数据没有在这次操作中被删除，也没有自动导入 Giscus，数据库实际完整性仍需单独确认。

LeanCloud 已宣布将在 **2027 年 1 月 12 日**停止对外服务，因此旧评论应尽早尝试恢复并导出，不能将暂时恢复访问当成长久方案。[LeanCloud 官方公告](https://docs.leancloud.app/sdk/announcements/sunset-announcement/)

## 新建和更新文章

项目使用的是目录型文章模板。新建文章时执行：

```bash
hugo new content --kind post post/my-new-note
```

这会使用 `archetypes/post/`，创建 `content/post/my-new-note/index.md`。如果直接执行 `hugo new content post/my-new-note/index.md`，虽然也能创建文件，但不会按目录型模板生成这套完整的字段。

编辑生成的 Front Matter：

```yaml
---
title: "我的新笔记"
date: "2026-11-12T12:00:00+08:00"
draft: true
description: "文章简介"
slug: "my-new-note"
image: "cover.jpg"
categories:
    - 博客搭建
tags:
    - Hugo
---
```

只有使用配图时才填写 `image`，并把 `cover.jpg` 放在文章同一目录。正文中可以用相对路径引用同目录图片。

写完后将 `draft` 改成 `false`。更新旧文章时保留原 `date`，新增或修改 `lastmod`；尽量保持 `slug` 不变，避免旧链接和评论关联失效。

## 构建与本地检查

在项目根目录执行：

```bash
hugo --gc --minify --panicOnWarning
python3 scripts/verify_site.py
python3 scripts/verify_comments.py
node scripts/verify_giscus_theme.cjs
```

几个检查分别负责：

- 检查迁移时记录的正文和图片快照、117 个旧 HTML/XML/图片地址，以及生成页面中的站内链接和资源。
- 确认原来的 8 篇文章仍在搜索索引中，同时允许新增文章。
- 检查每篇文章的 Giscus 仓库、分类、路径映射和语言设置，确保不再加载 Waline。
- 检查主题切换及无关消息过滤，不会发送测试评论。

`docs/migration-manifest.json` 中的文件哈希用于避免迁移后意外改坏旧内容。以后有意修改历史文章时，需要同步更新该文件对应的 SHA-256；这次重写本文也记录了更新前后的哈希。新增文章不受这份旧文件快照清单限制。

构建成功后，生成网页位于 `public/`。不要把预览草稿使用的 `-D` 加进正式发布命令。

## 使用 GitHub Actions 发布到 Pages

仓库的 **Settings → Pages → Build and deployment → Source** 要选择 **GitHub Actions**。

完整的[发布工作流](https://github.com/lixiaojun2914/lixiaojun2914.github.io/blob/main/.github/workflows/pages.yml)已经放在 `.github/workflows/pages.yml`，主要步骤是：

1. 检出当前提交。
2. 下载固定版本 Hugo 和 Dart Sass，并校验下载文件的 SHA-256。
3. 执行与本地一致的构建和校验命令。
4. 上传 `public/` 为 Pages 构建产物。
5. 使用 `actions/deploy-pages` 发布。

当前工作流固定 Hugo 0.169.0、Dart Sass 1.108.2，设置 `TZ = Asia/Shanghai`。更新版本时要同步更新工作流中的下载校验和。

`main` 分支负责发布；Pull Request 和 `migration/**` 分支只构建与验证。也可以在 Actions 页面手动运行工作流。使用仓库提供的 `GITHUB_TOKEN` 与 Pages 权限，不需要另存一个个人访问令牌。

如果当前使用 SSH 推送，可把远端设置为：

```bash
git remote set-url origin git@github.com:lixiaojun2914/lixiaojun2914.github.io.git
```

确认电脑的 SSH 密钥已关联到 GitHub，然后提交并推送：

```bash
git add content/
git commit -m "Update blog posts"
git push origin main
```

上面的命令适用于正在 `main` 分支上写作的情况。若同时修改了配置、工作流或快照哈希，也要把这些文件一起提交；分支开发时先走 Pull Request，检查通过后再合并到 `main`。

在仓库 **Actions → Build and deploy Hugo** 中确认 `build`、`deploy` 都成功，再访问 <https://lixiaojun2914.github.io/> 检查结果。工作流成功只说明构建部署完成，首次接入评论或改模板后，仍需要检查实际页面表现。

## 以后维护时注意什么

- 写新文章：使用目录型模板，把配图放在文章目录中，发布前关闭草稿状态。
- 改旧文章：保留 slug 和首次发布日期，补充 lastmod；有意修改快照中的旧文件时同步更新哈希。
- 升级 Hugo：同步修改本地版本、`.hugo-version` 与 Actions 中的版本和校验和。
- 升级 Stack：整体替换主题目录，保留许可证，复核根目录的兼容覆盖是否仍需要。
- 管理评论：到 GitHub Discussions 管理 Giscus 评论，保留旧 LeanCloud 数据的独立备份。

本文对应的配置与脚本均可在[博客源码仓库](https://github.com/lixiaojun2914/lixiaojun2914.github.io)查看，后续以仓库中的实际实现为准。
