# 依赖版本记录(dep)

应用与底层框架(aipassport-fw)的版本配对台账。框架以 git 子模块挂在
`components/framework`;每次更换框架提交并整机验证通过后,在这里补一行。

## 当前配对

| 日期 | 应用(aipassport-glm-usage) | 框架(aipassport-fw) | 说明 |
| --- | --- | --- | --- |
| 2026-10-05 | 迁移提交(见 git log "migrate to aipassport-fw") | `7c85191` | 首次迁移到统一框架;与收音机同一框架提交 |

- 迁移前:应用自带 app_net/app_netlist/app_portal/app_ui + components/bsp
  (框架的祖先副本,已删除,见 docs/migration-to-framework.zh_CN.md)
- 运行期核对:屏幕 设置 → 设备信息,或 MCP `get_device_info`,报告
  「框架 <fw短hash>.<日期> | 应用 <app短hash>」
- 升级注意:热点列表改存框架 "appfw" 命名空间,从旧版(自研存储,"glm" 命名
  空间)升级的设备需重新配网一次;GLM Key/组织/项目 ID 保留("glm" 命名空间
  未动),无需重填

## 规则(与 aipassport-radio/dep.md 一致)

1. 更新框架子模块后必须重新编译 + 真机验证,通过后才提交子模块指针并登记。
2. 应用提交含子模块指针变更时,dep.md 同一批更新。
3. 回滚 = 把子模块指回表中的框架提交,再整机重刷。
