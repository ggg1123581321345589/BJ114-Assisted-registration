# 第三方组件说明

适用版本：辅助挂号软件 v1.2.0。

项目自身沿用 LICENSE 中的 MIT 许可。第三方内容继续适用其原有许可，不能因本项目使用 MIT 而改写其许可条件。

## 随源码包含

- vendor/jsdom：jsdom 31.0.0，MIT；完整许可见 vendor/jsdom/LICENSE.txt。运行库文件与当前实际使用的版本一致；package.json 去除了仅上游开发构建使用的依赖与脚本，避免安装时重新生成已提供的运行源码。
- runtime/helpers：为当前程序配套的运行时兼容模块，不是官方浏览器实现。模块定位经过可移植调整；来源及文件哈希记录在本次整理的外部证据文件中。
- runtime/public_headers.json：公开请求头配置，未包含个人会话、Cookie、认证令牌或捕获的响应正文。
- data：公开医院和科室目录快照，用于目录加载。源码整理移除了开发机 source_file 路径。目录不附带医院数据、名称、商标或平台服务的额外授权。

## npm 安装的依赖

直接依赖版本固定在 package.json / package-lock.json：jsdom 31.0.0、whatwg-url 17.3.0、fake-indexeddb 6.2.7、@napi-rs/canvas 1.1.0、sm-crypto 0.5.8。安装后应保留各包附带的 LICENSE / NOTICE 文件；传递依赖及可选平台包以锁文件为准。

手机号加密适配器使用 sm-crypto 与 jsbn 的 MIT 实现。其公钥为服务端公开加密公钥，不是用户私钥或令牌；协议采用 SM2 C1C3C2，并添加 04 点前缀。本仓库未重新分发从站点提取且许可不明的压缩加密 JavaScript。

## 外部构建和运行依赖

Qt、libcurl、Node.js、MSVC 与各自依赖不作为 SDK 或二进制随本源码分发。构建或分发可执行程序时，需另行遵守所选版本及链接方式对应的许可、再分发条件和通知义务。不要仅依靠本文件替代组件自身的许可文本。

Windows 运行包另附 Qt、Node、Microsoft C++ 运行库和其他实际依赖，并在 THIRD_PARTY_LICENSES 中保留许可原文与组件来源索引。sm-crypto 的许可文件原名为 LICENCE_MIT，jsbn 为 LICENSE；两者应随依赖保留，不能只依靠包的 license 字段替代原文。

Windows 会话保存调用系统提供的 CurrentUser DPAPI，不将其作为本项目自带加密库重新分发。源码不包含真实账号、会话文件或抓包；组件许可也不构成官方平台账号、数据或服务的使用授权。
