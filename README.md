# 辅助挂号软件v1.0.0

## 免责说明

本软件仅用于辅助查询公开号源，不提供自动预约或代挂号服务，不保证号源可用或挂号成功。信息可能延迟或变化，请以医院或官方平台为准，并通过官方渠道办理挂号。请合理使用，遵守平台规则，勿用于倒号或其他违规用途。完整说明见 [免责声明](docs/免责声明.md)。

这是截图中 C++ / Qt5 号源查询版的源码整理版本。窗口标题为“辅助挂号软件v1.0.0”，界面第一行为“免责说明”。本仓库准备供作者上传 GitHub；不包含已编译程序、第三方 DLL、本机抓包或个人会话。

## 功能

- 按医院名称、地区或编码搜索医院。
- 从本地公开快照加载科室，支持全院科室采集、停止与断点续跑。
- 按医院、科室和单日查询实时号源，展示医生、时段、剩余号源、状态与查询时间。
- 后台处理网络任务，支持连接预热和停止操作。
- 可选成功后重复刷新，间隔为 60–3600 秒；查询失败后停止重复刷新。

医院、科室目录属于本地资料，可能过时；实时号源在查询时获取，不将旧号源缓存当作新结果。切换医院、科室或日期会清空当前号源展示。本版本没有登录、短信验证、订单提交或自动抢号功能。

## 源码结构

```text
src/                      C++ 窗口、后台服务和 HTTP 传输源码
runtime/                  查询所需 JavaScript 运行时与兼容适配器
vendor/jsdom/             当前使用的 jsdom 源码及原许可
data/                     公开医院目录及可选科室快照
tests/                    离线 C++ 测试
docs/                     使用说明与免责声明
CMakeLists.txt            CMake 工程
AssistedRegistration.pro  qmake 工程
build.cmd                 Windows 构建入口
run.cmd                   Windows 启动入口
package.json              JavaScript 依赖与离线自检命令
package-lock.json         npm 依赖锁定文件
THIRD_PARTY_NOTICES.md     第三方组件说明
```

程序需要 `data/hospitals.csv`。`data/hospital_departments_snapshot.json` 是公开科室快照，可由界面的“采集全院科室（断点续跑）”生成；采集访问网络，应合理使用。运行生成的缓存、采集进度及日志位于 `output/`，不属于待上传源码。

## 开发环境

- Windows x64，支持 C++17 的 MSVC 工具链（Visual Studio / Build Tools）。
- 与 MSVC 工具链及架构匹配的 Qt 5.15 SDK，含 Core、Gui、Widgets、Network；离线 UI 测试另需 Qt Test。
- libcurl 开发库，需与所选编译器、架构和运行库兼容。
- Node.js 与 npm：版本要求见 `package.json` 的 `engines.node`。
- CMake 3.20 或更新版本；使用 qmake 工程时需要 qmake、nmake。

C++ 负责界面与业务，查询运行时仍依赖 Node.js，不是无需任何运行时依赖的单文件程序。源码包不包含 Qt、libcurl 或 Node 安装程序。

## 构建与启动

在仓库根目录打开命令提示符，设置当前机器的 SDK 路径，再安装锁定依赖并构建：

```bat
set "QT_ROOT=C:\Qt\5.15.2\msvc2019_64"
set "CURL_ROOT=C:\SDK\curl"
npm ci
build.cmd
run.cmd
```

路径仅为占位示例，须替换为自己的实际安装目录。`build.cmd` 使用本机 MSVC 环境和 Qt 工具；也可在已配置的 x64 Native Tools 命令提示符中运行。若使用自定义 Visual Studio 安装路径，可设置 `VCVARS64` 为对应的 `vcvars64.bat`。编译成功后显示 EXE 的绝对路径和大小，目标是仓库内的 `bin/AssistedRegistration.exe`，不是仓库根目录。完整输出写入 `build.log`；失败时也保留窗口，须查看日志中的实际错误，不能把旧 EXE 当作新构建。命令行自动化可使用 `build.cmd --no-pause`。Qt 插件与依赖 DLL 必须可供 Windows 加载，发布运行目录时还需按所用 SDK 部署；不能只复制 exe。

`run.cmd` 以仓库根目录作为 `--project-root`。程序优先使用 `NODE_EXECUTABLE` 指定的 Node；未指定时先查找运行目录的 `runtime/node/node.exe`，再从 PATH 查找。可在启动前设置：

```bat
set "NODE_EXECUTABLE=C:\Program Files\nodejs\node.exe"
run.cmd
```

不要移动或修改 `runtime/query_runtime.cjs` 后忽略后端的完整性校验；运行时源码与 C++ 中记录的摘要必须一致。

## 使用步骤

1. 启动程序并阅读顶部免责说明；启用连接预热时等待连接准备完成。
2. 搜索并选中医院，加载科室。未覆盖所选医院时，按界面提示采集科室资料。
3. 选择科室和查询日期，点击“查询实时号源”。
4. 如需重复刷新，勾选相应选项并设置不少于 60 秒的间隔；可随时点击“停止”。
5. 挂号请前往医院或官方平台完成，并再次核对实际号源。

网站响应、访问限制或接口调整可能导致查询失败。出现失败时根据界面提示处理；不要把空白、旧记录或失败状态视为医院没有号源。

## 离线检查

JavaScript 语法与运行时自检：

```bat
npm run check
npm test
```

启用 CMake 测试后运行 C++ 离线测试：

```bat
cmake -S . -B build-cmake -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH="%QT_ROOT%" -DCURL_ROOT="%CURL_ROOT%"
cmake --build build-cmake --config Release
ctest --test-dir build-cmake -C Release --output-on-failure
```

本机已验证 qmake + nmake 构建，以及纯英文路径隔离副本的 CMake 构建和 4 个 CTest 测试。当前机器的 CMake + NMake 在中文路径生成编译探针时出现 PDB 路径编码错误；使用 CMake 时建议将完整源码放入纯英文路径，并与 qmake 使用不同构建目录。离线测试使用本地测试数据，不查询医院网站，也不证明某一时刻线上号源可用。实际测试结果以本次构建输出和验证记录为准。

## 上传 GitHub

请将本目录作为仓库根目录。`.gitignore` 已排除构建产物、`node_modules/`、运行输出和常见本机文件。上传前检查暂存列表，确认没有账号、Cookie、Token、个人信息、抓包、日志或私有配置。不要把整个开发工作目录一起上传。

下面是供作者自行执行的命令，`REPOSITORY_URL` 替换为自己的 GitHub 仓库地址：

```bat
git init
git add .
git status --short
git diff --cached --stat
git commit -m "Release auxiliary registration query v1.0.0"
git branch -M main
git remote add origin REPOSITORY_URL
git push -u origin main
```

本次整理不自动创建远端仓库或执行推送。若已有 Git 仓库或远端配置，请使用既有配置。

## 许可与第三方组件

本次整理没有替作者选择项目级开源许可证；`package.json` 标记为 `UNLICENSED`，未额外授予项目代码的使用、修改或分发许可。若计划以开源项目发布，请由权利人确认授权范围并添加相应 `LICENSE`。第三方组件仍适用其原有许可，详见 [第三方组件说明](THIRD_PARTY_NOTICES.md) 和各组件许可文件。
