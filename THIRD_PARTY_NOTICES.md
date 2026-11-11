# 第三方组件与来源

- Qt 5 与 libcurl：通过开发者安装的 SDK 链接。源码仓库不包含其 DLL；分发程序时应遵循各上游组件的许可要求。
- jsdom 31.0.0：vendor/jsdom 保存项目实际使用的 DOM 实现源码，许可见 vendor/jsdom/LICENSE.txt（MIT）。npm 使用本地 file:vendor/jsdom；传递依赖由 package-lock.json 固定。
- whatwg-url 17.3.0、fake-indexeddb 6.2.7、@napi-rs/canvas 1.1.0 及其传递依赖：通过 npm 安装，许可见各 npm 包。
- runtime/helpers：现用 DOM 兼容适配器源码；不包含个人账号、Cookie 值、会话记录、完整抓包或从服务器保存的安全脚本。
- runtime/public_headers.json：仅包含固定公开浏览器请求头，不包含身份认证头或会话 Cookie。

本目录是截图中的号源查询版，仅整理其查询运行时，没有引入登录、短信或订单提交功能。未代替作者选择统一开源许可证；package.json 使用 UNLICENSED。第三方组件原有许可始终适用。

## jsdom 运行分发元数据调整

保留 lib/ 中实际运行源码、预生成接口、README.md 与 LICENSE.txt。vendor/jsdom/package.json 删除仅供上游仓库开发的 scripts、wireit 和 devDependencies，使本地源码依赖可直接 npm ci，无需不存在的上游生成脚本。DOM 运行源码未在本次整理中替换。
