All actions taken by users while using this software and during the process of obtaining developer permissions, as well as any consequences arising from such actions (including but not limited to legal risks and breach of contract liabilities), are entirely borne by the users themselves. The developers of this software strictly prohibit the use of this software for any actions that violate applicable laws and regulations, undermine the fairness of games, violate third-party service terms (TOS), or infringe upon the legitimate rights of third parties. If users engage in any improper, non-compliant, or illegal activities using this software, such actions are solely at the users' own discretion and independent operation, and have no connection with the software or its developers. The developers strongly condemn such behavior and do not assume any direct or joint legal or financial responsibility for any improper actions of the users.

## 便捷快捷键

参照 DetachedPlugins/ConvenienceTools 添加以下设置，默认关闭；加载已有配置时仅补充缺失的设置节，保留用户值与自定义快捷键。

| 功能 | 启用设置 | 快捷键设置 | 默认按键 |
| --- | --- | --- | --- |
| 快捷领取每日委托 | `QuickClaim` | `ClaimAllKey` | F8（119） |
| 快捷打开锻造界面 | `AutoForge` | `AutoForgeKey` | F7（118） |

将启用设置的 `Value` 改为 `1` 即可启用。快捷键使用 `Type = key`，支持启动器的按键选择与十进制虚拟键值；`0` 禁用该快捷键。完整设置片段见 `config_add.txt`。

按住按键不会重复触发，切回游戏或修改绑定时已经按下的按键需要先松开。委托领取在游戏线程分两阶段发送请求，间隔至少 600 毫秒；客户端失效、切换或关闭功能时取消等待。委托后端仅支持通过版本头和函数入口校验的国服 7.1，其他版本自动禁用。锻造入口复用现有页面路由，并检查当前是否允许打开界面。

本地回归入口为 `tools/ShortcutRegression.vcxproj`（Release x64），运行生成的 `ShortcutRegression.exe` 时传入一个已有的独立临时目录。该回归仅检查本地调度、内存访问和配置升级，游戏内领取结果与锻造界面仍需实际验证。
