# 高德 Web 服务接口参考

本文只记录本 Skill 实际用到的接口与字段，完整文档见 https://lbs.amap.com/api/webservice/summary 。

## Key 与配额

- 申请：https://lbs.amap.com/api/webservice/create-project-and-key ，Key 类型选择「Web 服务」。
- 代码读取 `AMAP_KEY`，兼容 `AMAP_WEBSERVICE_KEY`、`AMAP_WEB_SERVICE_KEY`。
- JS 地图额外需要「Web 端 (JS API)」Key，读取 `AMAP_JS_KEY`；v2.0 需要安全密钥 `AMAP_JS_SECURITY_CODE`。
- 配额与 QPS 以控制台为准。一次 `discover` 约消耗：锚点检索 3×页数 + 候选数×8 次请求（候选默认 8 个时约 70 次）。
- 重复分析同一区域时加 `--cache-dir .amap-cache`，缓存按完整 URL 的 SHA1 存放。

## 用到的接口

| 用途 | 路径 | 关键参数 |
|---|---|---|
| 城市内锚点检索 | `/v3/place/text` | `keywords`、`city`、`citylimit=true`、`offset=25`、`page` |
| 地址转坐标 | `/v3/geocode/geo` | `address`、`city` |
| 周边 POI 统计 | `/v3/place/around` | `location`、`radius`、`types`、`keywords`、`offset`、`page` |
| 交通态势 | `/v3/traffic/status/circle` | `location`、`radius`、`extensions=base` |

所有接口统一返回：

```json
{ "status": "1", "info": "OK", "infocode": "10000", "count": "123", "pois": [] }
```

`status != "1"` 时脚本抛错并带上 `infocode`、`info`。

## 低成本计数技巧

`/v3/place/around` 的 `count` 是命中总数，与 `offset` 无关。统计某一类 POI 密度时用 `offset=1`，一次请求即可拿到总数，不需要翻页；需要竞品名称样例时才用 `offset=25`。

## POI 编码

| 类别 | 编码 |
|---|---|
| 购物中心 | `060100` |
| 便利店 | `060200` |
| 餐饮服务 | `050000` |
| 咖啡厅 | `050500` |
| 商务写字楼 | `120201` |
| 住宅区 | `120300` |
| 学校 | `141200` |
| 地铁站 | `150500` |
| 公交站 | `150700` |

`keywords` 支持用 `|` 分隔多个关键词；`types` 支持用 `|` 分隔多个编码。编码可能随高德版本调整，异常时先用控制台或 `/v3/config/district` 核对。

## 交通态势返回

```json
{
  "status": "1",
  "trafficinfo": {
    "evaluation": {
      "status": "畅通",
      "description": "道路畅通，平均车速 45km/h"
    }
  }
}
```

脚本把「畅通 / 缓行 / 拥堵 / 严重拥堵」映射为 100 / 70 / 40 / 10，未知按 60 计。

## 地图链接与可视化

- 无需 Key 的标记链接：`https://uri.amap.com/marker?position={lng},{lat}&name={name}&coordinate=gaode&callnative=0`
- 报告内地图：`https://webapi.amap.com/maps?v=2.0&key={AMAP_JS_KEY}`，并在加载前设置 `window._AMapSecurityConfig = { securityJsCode: "..." }`。
- 未配置 JS Key 时报告只输出卡片和 URI 链接，不影响功能。

## 常见错误码

| infocode | 含义 | 处理 |
|---|---|---|
| `10001` | `INVALID_USER_KEY` | Key 错误或未启用 Web 服务 |
| `10003` | `DAILY_QUERY_OVER_LIMIT` | 当日配额用尽 |
| `10004` | `CUQPS_HAS_EXCEEDED_THE_LIMIT` | QPS 超限，降低并发或加缓存 |
| `10009` | `USERKEY_PLAT_NOMATCH` | Key 平台类型不匹配，需 Web 服务 Key |
| `10011` | `INSUFFICIENT_PRIVILEGES` | 接口权限不足 |
| `20000` | `INVALID_PARAMS` | 参数缺失或格式错误 |
| `20800` | `OUT_OF_SERVICE` | 区域无数据或服务不可用 |
