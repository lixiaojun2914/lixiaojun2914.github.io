---
title: 关于
description: "关于这个技术博客、文章怎么写，以及从哪里开始读。"
slug: about
aliases: [/about-us/, /about-hugo/, /contact/]
menu:
    main:
        name: 关于
        weight: -50
        params:
            icon: user
comments: false
---

这是我的技术博客，用来记录学习和实验中遇到的问题。我喜欢从一个可以复现的现象出发，先看命令或程序跑出了什么，再回到源码，弄清楚内核为什么会这样做。

## 一、从哪里开始读

目前可以从 Linux 文件系统系列开始。前几篇从根目录和挂载讲起，逐步追到打开文件时的内核对象，最后通过 ext4 镜像和权限实验回到实际操作。

- [从根目录看目录树与挂载](/p/linux-root-filesystem/)
- [从两次 open 理解内核对象模型](/p/linux-file-object-model/)
- [从 inode 编号找到 ext4 的数据块](/p/linux-ext4-disk-layout/)
- [从 chmod 和 umask 到 VFS 权限检查](/p/linux-file-permissions/)

博客还会陆续记录 Linux 与网络、数学基础、机器学习与深度学习、Agent 工程、容器与 Kubernetes，以及数据结构与算法。想看整体计划，可以读[博客建设与写作计划](/p/blog-roadmap/)。找已发布的文章，可以从[归档](/archives/)或[分类](/categories/)进入。

## 二、订阅与交流

可以把 [RSS 地址](/index.xml)添加到阅读器。文章里如果有步骤跑不通，或源码解释需要补充，欢迎在对应文章下留言，也可以到[博客仓库](https://github.com/lixiaojun2914/lixiaojun2914.github.io)反馈。常用的文档和源码入口整理在[链接](/links/)页。
