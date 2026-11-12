# 最新查询连接调度离线测试

这些测试只使用合成事件与公有医院目录。保留现有基础 UI、服务与 parser 测试。

- pool_recovery_offline_test.cpp：真实 Window/QThread/QTimer，维护目标、备用连接、到期轮换、额度轮换、最多两个维护补连、2–60 秒退避、Stop/Close、旧 epoch、失败业务不重放。所有 Worker 业务信号断连。
- parallel_query_offline_test.cpp：七天查询的异步 QTimer fixture；支持 PROBE_CONNECTIONS=1/2/4/16、PROBE_READY_COUNT 与 PROBE_MODE=detail/empty/mixed/fastdetail，检查独立 owner、并发上限、日期顺序与号源实时计划。数值是 fixture 调度时间，不能当作网站速度。
- offline_broker_guard.cpp：专用于上述两个测试的拒网实现，DNS 预取为空操作，意外业务调用失败。生产程序仍使用真实 native_backend.cpp。
- dns_cache_offline_test.cpp：从对应后端版本提取的 DNS 共享/缓存/等待 contract，用固定答案与延迟替代系统 lookup；不证明真实 Windows DNS 或传输行为。

构建：在项目内建立独立 build-tests/TEST_NAME，moc ../../src/main.cpp 到 generated/main.moc（两个 Window 测试），然后 qmake ../../tests/latest-offline.pro TEST_NAME=TEST_NAME 与 nmake。Qt5 SDK、VC 编译环境由调用者配置。DNS 测试不需要 moc。运行 Window 测试时传入项目根目录；可设 QT_QPA_PLATFORM=windows。勿把构建目录和测试输出入仓。
