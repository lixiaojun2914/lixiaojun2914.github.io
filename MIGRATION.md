# 迁移记录（2026-11-12）

## 来源与保留内容

- 发布仓库只有 Hugo 0.92.0 + Stack 3.7.0 的生成结果，没有完整配置与主题源码。
- 文章仓库 `lixiaojun2914/blog` 包含 8 篇 Markdown、2 张配图。原始文件逐字保留，哈希记录在 `docs/migration-manifest.json`。
- 从旧发布站恢复头像、About 正文、链接页数据、Test 分类说明及图片、旧图片 URL。
- 保留所有文章日期、标题、slug、分类和标签。`C-lang` 仍输出 `/p/c-lang/`；中文 `/p/哈哈哈哈/` 也保持原样。
- `/about-us/`、`/about-hugo/`、`/contact/` 仍跳转到 `/about/`。
- 保留 My Blog 站名、2020 页脚起始年与旧文章授权展示。没有把 Hugo 介绍页改写为个人简历。

## 目录评估

原来“一个目录一篇文章 + 配图”的方式符合 Hugo Leaf Bundle，不需要拆散。问题是站点源码与发布文件分开后，缺少可重复构建的完整工程。

现在统一为完整 Hugo 项目；文章放 `content/post/`、工具页放 `content/page/`，配置按职责拆分。主题固定版本随项目保存；少量覆盖放根目录 `layouts/`，避免修改主题本体。`public/`、缓存和编译资源不纳入源码管理。

## 新版兼容处理

| 旧设置/情况 | 处理 |
| --- | --- |
| `paginate: 5` | `[pagination] pagerSize = 5` |
| 旧语言字段与 `zh-cn` 主题语言 | `locale = "zh-CN"`、`defaultContentLanguage = "zh"`，匹配 Stack 4 的中文翻译 |
| `hasCJKLanguage: false` | 改为 `true`，适应中文摘要和字数 |
| 头像对象 `enabled/local/src` | Stack 4 使用 `avatar = "img/avatar.png"` |
| `widgets.enabled` | 改为 `widgets.homepage` 与 `widgets.page` |
| LibSass 默认编译 | 覆盖 `head/style.html`，显式使用 Dart Sass；定向静默上游 import/global-builtin 弃用提示 |
| 旧首页新标签打开 | 去掉首页 `newTab: true`，站内导航在当前标签打开 |
| Google Fonts | 改为中文系统字体，无需访问字体服务 |
| Markdown 中的 `<br>` 等 HTML | 保留受信任原文渲染，启用 Goldmark `unsafe`；About 恢复为 Markdown |
| 公式与代码 | 保留 KaTeX 配置、开启 passthrough、代码高亮、复制按钮及目录 |
| 旧图片处理字段 | 使用 Stack 4 默认图像处理参数，并保留旧图片文件供历史 URL 使用 |
| 无自动化部署源码 | 添加固定版本与 SHA-256 下载校验的原生 GitHub Pages workflow |

配置依据来自文章《Hugo Blog搭建过程》中的旧配置示例及已发布页面，不能等同于拿到了当年的真实配置文件。

## 外部服务与历史内容注意事项

1. **Waline**：旧地址仍在，但 2026-11-12 读取 `/comment` 返回 HTTP 500，提示 LeanCloud 应用已归档。`/api/comment` 则为 404（旧版接口路径）。因此保留地址并将 `comments.enabled` 暂设为 `false`。旧评论存储未迁移、未删除。恢复后需要验证服务端与 Stack 所用客户端的版本兼容，再开启评论。
2. **失效图片**：Markdown 笔记引用 `tenfei04.cfp.cn/.../VCG211291536009.jpg`，检查时 DNS 解析失败。保留原文与 URL，外链图片不参与构建期下载，避免阻塞部署。该图片在浏览器中仍可能无法显示；需要找回原图才能完整修复。
3. **空白文章**：`Pytorch笔记` 原文仅有元数据，描述和标签写的是 matplotlib；按原样保留，未凭空添加内容。
4. **主题示例**：Rich Content、Test 分类和 About 的 Hugo 介绍属于历史示例内容；保留 URL 与正文，后续可以自行清理。
5. **旧教程**：《Hugo Blog搭建过程》内的命令与 YAML 是历史正文，按原样保留；当前操作请用 README，避免误把旧配置复制回新项目。
6. **浏览器外部依赖**：主题的图片灯箱、公式和 Bilibili 嵌入仍依赖对应 CDN/第三方站点，未声称完全离线可用。

## 验证方式

- 使用校验过官方 SHA-256 的 Hugo 0.169.0 Extended 与 Dart Sass 1.108.2 构建。
- `hugo --gc --minify --panicOnWarning` 检查当前适配后的构建。
- `python3 scripts/verify_site.py` 检查原始文件哈希、旧 HTML/XML/图片路径、全部生成 HTML 的站内链接/资源以及 8 篇文章的中文全文搜索数据。
- 线上部署需要完整源码位于原仓库的 main 分支，并选择 GitHub Actions 作为 Pages 来源；migration/** 分支仅执行构建与校验。
