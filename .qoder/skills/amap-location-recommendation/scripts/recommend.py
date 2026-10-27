#!/usr/bin/env python3
"""高德区位推荐：自动发现候选商圈，或对给定地址做多维打分排序。"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import math
import os
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

API_BASE = "https://restapi.amap.com/v3"
SKILL_DIR = Path(__file__).resolve().parent.parent
TEMPLATE_PATH = SKILL_DIR / "assets" / "report_template.html"

KEY_ENV_NAMES = ("AMAP_KEY", "AMAP_WEBSERVICE_KEY", "AMAP_WEB_SERVICE_KEY")

ANCHOR_SPECS = (
    ("mall", "购物中心", 3),
    ("office", "写字楼", 2),
    ("metro", "地铁站", 1),
)

CATEGORY_TYPES = {
    "mall": "060100",
    "office": "120201",
    "residential": "120300",
    "school": "141200",
    "metro": "150500",
    "bus": "150700",
}

CATEGORY_LABELS = {
    "mall": "购物中心",
    "office": "写字楼",
    "residential": "住宅区",
    "school": "学校",
    "metro": "地铁站",
    "bus": "公交站",
}

# 用于把 POI 计数压到 0-100：达到 cap 即视为满分，避免核心商圈数值失控。
CATEGORY_CAPS = {
    "mall": 8,
    "office": 120,
    "residential": 80,
    "school": 10,
    "metro": 4,
    "bus": 25,
}

PROFILES = {
    "coffee": {
        "label": "咖啡",
        "competitor_keywords": "",
        "competitor_types": "050500",
        "demand": {"office": 0.40, "mall": 0.30, "residential": 0.20, "school": 0.10},
        "competition": {"mode": "cluster", "ideal": 12, "saturation": 40},
    },
    "tea": {
        "label": "奶茶",
        "competitor_keywords": "奶茶|茶饮|饮品",
        "competitor_types": "",
        "demand": {"mall": 0.40, "school": 0.25, "residential": 0.20, "office": 0.15},
        "competition": {"mode": "cluster", "ideal": 15, "saturation": 45},
    },
    "restaurant": {
        "label": "餐饮",
        "competitor_keywords": "",
        "competitor_types": "050000",
        "demand": {"mall": 0.35, "office": 0.30, "residential": 0.25, "school": 0.10},
        "competition": {"mode": "cluster", "ideal": 25, "saturation": 80},
    },
    "convenience": {
        "label": "便利店",
        "competitor_keywords": "",
        "competitor_types": "060200",
        "demand": {"residential": 0.45, "office": 0.30, "mall": 0.15, "school": 0.10},
        "competition": {"mode": "avoid", "ideal": 0, "saturation": 15},
    },
    "gym": {
        "label": "健身房",
        "competitor_keywords": "健身|健身房|健身中心",
        "competitor_types": "",
        "demand": {"residential": 0.45, "office": 0.35, "mall": 0.20, "school": 0.0},
        "competition": {"mode": "avoid", "ideal": 0, "saturation": 8},
    },
    "generic": {
        "label": "通用",
        "competitor_keywords": "",
        "competitor_types": "",
        "demand": {"mall": 0.25, "office": 0.25, "residential": 0.25, "school": 0.25},
        "competition": {"mode": "avoid", "ideal": 0, "saturation": 20},
    },
}

ALIASES = {
    "咖啡": "coffee",
    "咖啡厅": "coffee",
    "咖啡馆": "coffee",
    "奶茶": "tea",
    "茶饮": "tea",
    "饮品": "tea",
    "餐饮": "restaurant",
    "餐厅": "restaurant",
    "饭店": "restaurant",
    "快餐": "restaurant",
    "便利店": "convenience",
    "健身房": "gym",
    "健身": "gym",
    "通用": "generic",
}

TRAFFIC_SCORES = {"畅通": 100, "缓行": 70, "拥堵": 40, "严重拥堵": 10}
DISCLAIMER = "POI 数量与交通态势来自高德开放平台，是需求与竞争的代理指标；不含租金、人口、销售额和真实客流，分数仅用于初筛。"


class AmapError(RuntimeError):
    pass


def _default_http_get(url: str) -> dict:
    request = urllib.request.Request(
        url, headers={"User-Agent": "seekdb-amap-location-recommendation/1.0"}
    )
    try:
        with urllib.request.urlopen(request, timeout=25) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise AmapError(f"HTTP {exc.code}") from exc
    except urllib.error.URLError as exc:
        raise AmapError(f"网络错误：{exc.reason}") from exc
    except json.JSONDecodeError as exc:
        raise AmapError("高德返回了非 JSON 响应") from exc


HTTP_GET = _default_http_get


class Client:
    def __init__(self, key: str, cache_dir: Path | None = None, delay: float = 0.05):
        self.key = key
        self.cache_dir = cache_dir
        self.delay = delay
        self.warnings: list[str] = []
        self._last_call = 0.0

    def warn(self, message: str) -> None:
        if message not in self.warnings:
            self.warnings.append(message)

    def get(self, path: str, params: dict) -> dict:
        query = {key: value for key, value in params.items() if value not in (None, "")}
        query["key"] = self.key
        query["output"] = "json"
        url = f"{API_BASE}{path}?" + urllib.parse.urlencode(
            query, quote_via=urllib.parse.quote, safe="|,"
        )

        cache_file = None
        if self.cache_dir is not None:
            cache_file = self.cache_dir / (
                hashlib.sha1(url.encode("utf-8")).hexdigest() + ".json"
            )
            if cache_file.exists():
                return json.loads(cache_file.read_text(encoding="utf-8"))

        wait = self.delay - (time.monotonic() - self._last_call)
        if wait > 0:
            time.sleep(wait)
        data = HTTP_GET(url)
        self._last_call = time.monotonic()

        if not isinstance(data, dict) or str(data.get("status", "")) != "1":
            code = data.get("infocode", "") if isinstance(data, dict) else ""
            info = data.get("info", "") if isinstance(data, dict) else ""
            raise AmapError(f"{code} {info}".strip() or "未知错误")

        if cache_file is not None:
            cache_file.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
        return data


def as_text(value) -> str:
    return value if isinstance(value, str) else ""


def parse_location(value: str):
    try:
        lng_text, lat_text = str(value).split(",")
        return float(lng_text), float(lat_text)
    except (ValueError, AttributeError):
        return None


def distance_m(a, b) -> float:
    lat1, lon1 = math.radians(a[1]), math.radians(a[0])
    lat2, lon2 = math.radians(b[1]), math.radians(b[0])
    dlat, dlon = lat2 - lat1, lon2 - lon1
    h = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    return 2 * 6371000 * math.asin(math.sqrt(h))


def resolve_profile(value: str):
    text = (value or "coffee").strip()
    key = ALIASES.get(text, text.lower())
    if key in PROFILES:
        return key, PROFILES[key]
    profile = dict(PROFILES["generic"])
    profile["label"] = text
    profile["competitor_keywords"] = text
    profile["competitor_types"] = ""
    return "custom", profile


def search_anchors(client: Client, city: str, pages: int):
    anchors = []
    for kind, keyword, weight in ANCHOR_SPECS:
        for page in range(1, pages + 1):
            data = client.get(
                "/place/text",
                {
                    "keywords": keyword,
                    "city": city,
                    "citylimit": "true",
                    "offset": 25,
                    "page": page,
                    "extensions": "base",
                },
            )
            pois = data.get("pois") or []
            for poi in pois:
                point = parse_location(poi.get("location", ""))
                if point is None:
                    continue
                anchors.append(
                    {
                        "name": as_text(poi.get("name")) or keyword,
                        "address": as_text(poi.get("address")),
                        "district": as_text(poi.get("adname")),
                        "location": poi.get("location"),
                        "kind": kind,
                        "weight": weight,
                        "lng": point[0],
                        "lat": point[1],
                    }
                )
            if len(pois) < 25:
                break
    return anchors


def cluster_anchors(anchors, max_candidates: int, min_sep_m: int = 1200, cell_deg: float = 0.006):
    buckets = {}
    for anchor in anchors:
        key = (math.floor(anchor["lng"] / cell_deg), math.floor(anchor["lat"] / cell_deg))
        buckets.setdefault(key, []).append(anchor)

    clusters = []
    for group in buckets.values():
        weight = sum(item["weight"] for item in group)
        lng = sum(item["lng"] * item["weight"] for item in group) / weight
        lat = sum(item["lat"] * item["weight"] for item in group) / weight
        top = max(group, key=lambda item: (item["weight"], len(item["name"])))
        clusters.append(
            {
                "name": top["name"],
                "address": top["address"],
                "district": top["district"],
                "location": f"{lng:.6f},{lat:.6f}",
                "lng": lng,
                "lat": lat,
                "anchor_weight": weight,
                "anchor_count": len(group),
            }
        )

    clusters.sort(key=lambda item: (item["anchor_weight"], item["anchor_count"]), reverse=True)
    selected = []
    for cluster in clusters:
        if all(
            distance_m((cluster["lng"], cluster["lat"]), (item["lng"], item["lat"])) >= min_sep_m
            for item in selected
        ):
            selected.append(cluster)
        if len(selected) >= max_candidates:
            break
    return selected


def geocode(client: Client, address: str, city: str):
    params = {"address": address}
    if city:
        params["city"] = city
    data = client.get("/geocode/geo", params)
    geocodes = data.get("geocodes") or []
    if not geocodes:
        raise AmapError(f"地址解析失败：{address}")
    item = geocodes[0]
    point = parse_location(item.get("location", ""))
    if point is None:
        raise AmapError(f"地址缺少坐标：{address}")
    return {
        "name": address,
        "address": as_text(item.get("formatted_address")) or address,
        "district": as_text(item.get("district")),
        "location": item["location"],
        "lng": point[0],
        "lat": point[1],
    }


def around(client: Client, location: str, radius: int, types: str = "", keywords: str = "", samples: bool = False):
    params = {
        "location": location,
        "radius": int(radius),
        # offset=1 也会返回 count 总数，用于低成本统计；需要样例时才取 25 条。
        "offset": 25 if samples else 1,
        "page": 1,
        "extensions": "base",
    }
    if types:
        params["types"] = types
    if keywords:
        params["keywords"] = keywords
    data = client.get("/place/around", params)
    try:
        count = int(data.get("count") or 0)
    except (TypeError, ValueError):
        count = 0
    examples = []
    if samples:
        examples = [as_text(poi.get("name")) for poi in (data.get("pois") or [])[:5]]
    return count, examples


def traffic(client: Client, location: str):
    try:
        data = client.get(
            "/traffic/status/circle",
            {"location": location, "radius": 1500, "extensions": "base"},
        )
    except AmapError as exc:
        client.warn(f"交通态势不可用：{exc}")
        return 60, "未知"
    evaluation = (data.get("trafficinfo") or {}).get("evaluation") or {}
    label = as_text(evaluation.get("status")) or "未知"
    return TRAFFIC_SCORES.get(label, 60), label


def category_score(count: int, cap: int) -> float:
    if count <= 0:
        return 0.0
    return min(100.0, 100.0 * math.log10(1 + count) / math.log10(1 + cap))


def demand_score(counts: dict, weights: dict) -> float:
    total = 0.0
    for category, weight in weights.items():
        if weight <= 0:
            continue
        total += weight * category_score(counts.get(category, 0), CATEGORY_CAPS[category])
    return round(total, 1)


def competition_score(count: int, config: dict) -> float:
    ideal = max(1, int(config["ideal"]))
    saturation = max(1, int(config["saturation"]))
    if config["mode"] == "cluster":
        if count <= ideal:
            return round(max(0.0, 100.0 - (ideal - count) / ideal * 60.0), 1)
        return round(max(0.0, 100.0 - (count - ideal) / ideal * 60.0), 1)
    return round(max(0.0, 100.0 - count / saturation * 100.0), 1)


def accessibility_score(counts: dict, traffic_value: float) -> float:
    metro = min(100.0, counts.get("metro", 0) / CATEGORY_CAPS["metro"] * 100.0)
    bus = min(100.0, counts.get("bus", 0) / CATEGORY_CAPS["bus"] * 100.0)
    return round(metro * 0.5 + bus * 0.2 + traffic_value * 0.3, 1)


def explain(counts, competitor_count, examples, traffic_label, profile, radius):
    contributions = []
    for category, weight in profile["demand"].items():
        if weight <= 0:
            continue
        score = category_score(counts.get(category, 0), CATEGORY_CAPS[category])
        contributions.append((weight * score, category))
    contributions.sort(reverse=True)

    reasons = [
        f"{CATEGORY_LABELS[category]} {counts.get(category, 0)} 个 / {radius}m"
        for _, category in contributions[:2]
    ]
    reasons.append(f"交通态势：{traffic_label}")
    if competitor_count:
        example_text = "、".join(examples[:3])
        reasons.append(f"同类竞品 {competitor_count} 家" + (f"（如 {example_text}）" if example_text else ""))

    risks = []
    if competitor_count >= profile["competition"]["saturation"] * 0.6:
        risks.append(f"竞品 {competitor_count} 家，存在饱和风险")
    if counts.get("mall", 0) <= 1 and counts.get("office", 0) <= 5:
        risks.append("核心客流设施偏少，需核实实际人流")
    return reasons, risks


def analyze(client: Client, candidate: dict, profile: dict, radius: int):
    counts = {}
    for category, typecode in CATEGORY_TYPES.items():
        try:
            count, _ = around(client, candidate["location"], radius, types=typecode)
        except AmapError as exc:
            client.warn(f"{candidate['name']} · {CATEGORY_LABELS[category]} 查询失败：{exc}")
            count = 0
        counts[category] = count

    competitor_specified = bool(profile["competitor_types"] or profile["competitor_keywords"])
    if competitor_specified:
        try:
            competitor_count, competitor_examples = around(
                client,
                candidate["location"],
                radius,
                types=profile["competitor_types"],
                keywords=profile["competitor_keywords"],
                samples=True,
            )
        except AmapError as exc:
            client.warn(f"{candidate['name']} · 竞品查询失败：{exc}")
            competitor_count, competitor_examples = 0, []
    else:
        competitor_count, competitor_examples = 0, []
        client.warn("未指定竞品关键词/类型，竞争度按中性 60 分处理")

    traffic_value, traffic_label = traffic(client, candidate["location"])

    demand = demand_score(counts, profile["demand"])
    competition = (
        competition_score(competitor_count, profile["competition"]) if competitor_specified else 60.0
    )
    access = accessibility_score(counts, traffic_value)
    total = round(demand * 0.55 + competition * 0.20 + access * 0.25, 1)
    reasons, risks = explain(counts, competitor_count, competitor_examples, traffic_label, profile, radius)

    result = {
        "name": candidate["name"],
        "address": candidate["address"],
        "district": candidate["district"],
        "location": candidate["location"],
        "lng": candidate["lng"],
        "lat": candidate["lat"],
        "scores": {
            "total": total,
            "demand": demand,
            "competition": competition,
            "accessibility": access,
        },
        "evidence": {
            "poi_counts": counts,
            "competitors": competitor_count,
            "competitor_examples": competitor_examples,
            "traffic": traffic_label,
        },
        "reasons": reasons,
        "risks": risks,
    }
    if "anchor_weight" in candidate:
        result["anchor_weight"] = candidate["anchor_weight"]
        result["anchor_count"] = candidate["anchor_count"]
    return result


def rank_candidates(items, city, business, profile_key, profile, radius, mode):
    items.sort(key=lambda item: item["scores"]["total"], reverse=True)
    for index, item in enumerate(items, 1):
        item["rank"] = index

    first = items[0]
    if len(items) > 1:
        gap = round(first["scores"]["total"] - items[1]["scores"]["total"], 1)
        summary = f"推荐 {first['name']}（{first['scores']['total']} 分），领先第 2 名 {gap} 分。"
    else:
        summary = f"推荐 {first['name']}（{first['scores']['total']} 分）。"

    return {
        "city": city,
        "business": business,
        "profile": profile_key,
        "profile_label": profile["label"],
        "radius_m": radius,
        "mode": mode,
        "generated_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "summary": summary,
        "disclaimer": DISCLAIMER,
        "candidates": items,
    }


def run_discover(args, client: Client):
    profile_key, profile = resolve_profile(args.business)
    anchors = search_anchors(client, args.city, args.anchor_pages)
    if not anchors:
        raise AmapError(f"在 {args.city} 没有检索到候选锚点，可改用 score 模式直接指定地址")
    candidates = cluster_anchors(anchors, args.max_candidates)
    analyzed = [analyze(client, candidate, profile, args.radius) for candidate in candidates]
    result = rank_candidates(
        analyzed, args.city, args.business, profile_key, profile, args.radius, "discover"
    )
    result["anchor_count"] = len(anchors)
    return result


def split_candidates(value: str):
    return [
        item.strip()
        for item in value.replace("，", ",").replace("、", ",").replace(";", ",").split(",")
        if item.strip()
    ]


def run_score(args, client: Client):
    profile_key, profile = resolve_profile(args.business)
    candidates = [geocode(client, item, args.city) for item in split_candidates(args.candidates)]
    analyzed = [analyze(client, candidate, profile, args.radius) for candidate in candidates]
    return rank_candidates(
        analyzed, args.city or "未指定", args.business, profile_key, profile, args.radius, "score"
    )


def escape_markdown(text: str) -> str:
    return text.replace("|", "\\|")


def render_markdown(result, html_path=None):
    mode = "自动发现" if result["mode"] == "discover" else "候选对比"
    lines = [
        f"## 区位推荐 · {result['city']} · {result['business']}",
        "",
        f"- 模式：{mode}",
        f"- 搜索半径：{result['radius_m']} m",
        f"- 生成时间：{result['generated_at']}",
        "",
        "| 排名 | 区位 | 综合 | 需求 | 竞争 | 可达 | 竞品 |",
        "|---:|---|---:|---:|---:|---:|---:|",
    ]
    for item in result["candidates"]:
        scores = item["scores"]
        lines.append(
            f"| {item['rank']} | {escape_markdown(item['name'])} | {scores['total']} | "
            f"{scores['demand']} | {scores['competition']} | {scores['accessibility']} | "
            f"{item['evidence']['competitors']} |"
        )

    lines.append("")
    for item in result["candidates"]:
        lines.append(f"### {item['rank']}. {item['name']}（{item['scores']['total']} 分）")
        lines.append(f"- 地址：{item['address'] or '—'}")
        lines.append(f"- 依据：{'；'.join(item['reasons'])}")
        if item["risks"]:
            lines.append(f"- 风险：{'；'.join(item['risks'])}")
        lines.append("")

    if result.get("warnings"):
        lines.append("**数据提示**：" + "；".join(result["warnings"]))
        lines.append("")
    lines.append(f"> {DISCLAIMER}")
    if html_path:
        lines.append("")
        lines.append(f"可视化报告：{html_path}")
    return "\n".join(lines)


def score_bar(label: str, value) -> str:
    width = max(0.0, min(100.0, float(value)))
    return (
        f'<div class="bar"><span>{label}</span>'
        f'<div class="track"><div class="fill" style="width:{width:.0f}%"></div></div>'
        f"<b>{value}</b></div>"
    )


def render_cards(result) -> str:
    chunks = []
    for item in result["candidates"]:
        scores = item["scores"]
        bars = "".join(
            [
                score_bar("需求", scores["demand"]),
                score_bar("竞争", scores["competition"]),
                score_bar("可达", scores["accessibility"]),
            ]
        )
        reasons = "".join(f"<li>{html.escape(reason)}</li>" for reason in item["reasons"])
        risks = ""
        if item["risks"]:
            risk_items = "".join(f"<li>{html.escape(risk)}</li>" for risk in item["risks"])
            risks = f'<ul class="risks">{risk_items}</ul>'
        link = "https://uri.amap.com/marker?" + urllib.parse.urlencode(
            {
                "position": item["location"],
                "name": item["name"],
                "src": "seekdb-location-recommendation",
                "coordinate": "gaode",
                "callnative": "0",
            },
            quote_via=urllib.parse.quote,
            safe=",",
        )
        chunks.append(
            f"""<article class="card">
  <div class="rank">{item["rank"]}</div>
  <div>
    <h2>{html.escape(item["name"])} <span class="score">{scores["total"]} 分</span></h2>
    <p class="addr">{html.escape(item["address"] or "地址未返回")}</p>
    <div class="bars">{bars}</div>
    <ul>{reasons}</ul>
    {risks}
    <p class="links"><a href="{html.escape(link)}" target="_blank" rel="noopener">在高德地图查看</a></p>
  </div>
</article>"""
        )
    return "\n".join(chunks)


def render_html(result, output_path: Path, js_key: str = "", security_code: str = ""):
    if not TEMPLATE_PATH.exists():
        raise SystemExit(f"缺少报告模板：{TEMPLATE_PATH}")

    warnings = ""
    if result.get("warnings"):
        warnings = (
            '<div class="warnings">数据提示：'
            + html.escape("；".join(result["warnings"]))
            + "</div>"
        )

    body = (
        f'<div class="summary">{html.escape(result["summary"])}</div>'
        f"{warnings}{render_cards(result)}"
        f'<p class="disclaimer">{html.escape(DISCLAIMER)}</p>'
    )

    if js_key:
        map_section = (
            '<section class="map-wrap"><div id="map"></div>'
            '<p class="hint">蓝圈为候选区位搜索半径，标记数字为排名。</p></section>'
        )
        points = [
            {
                "rank": item["rank"],
                "name": item["name"],
                "lng": item["lng"],
                "lat": item["lat"],
                "radius": result["radius_m"],
            }
            for item in result["candidates"]
        ]
        data_json = json.dumps(points, ensure_ascii=False).replace("<", "\\u003c")
        config = ""
        if security_code:
            config = f"<script>window._AMapSecurityConfig = {{ securityJsCode: {json.dumps(security_code)} }};</script>"
        map_script = f"""{config}
<script src="https://webapi.amap.com/maps?v=2.0&key={urllib.parse.quote(js_key, safe='')}"></script>
<script>
(function () {{
  if (!window.AMap) {{ return; }}
  var points = {data_json};
  var map = new AMap.Map("map", {{ zoom: 12, viewMode: "2D" }});
  points.forEach(function (point) {{
    new AMap.Circle({{
      center: [point.lng, point.lat],
      radius: point.radius,
      strokeColor: "#1677ff",
      strokeWeight: 1,
      fillColor: "#1677ff",
      fillOpacity: 0.06
    }}).setMap(map);
    new AMap.Marker({{
      position: [point.lng, point.lat],
      title: point.name,
      label: {{ content: point.rank + ". " + point.name, direction: "top" }}
    }}).setMap(map);
  }});
  map.setFitView();
}})();
</script>"""
    else:
        map_section = ""
        map_script = "<!-- 未设置 AMAP_JS_KEY，跳过地图渲染 -->"

    page = (
        TEMPLATE_PATH.read_text(encoding="utf-8")
        .replace("__TITLE__", html.escape(f"区位推荐 · {result['city']} · {result['business']}"))
        .replace(
            "__SUBTITLE__",
            html.escape(
                f"{result['profile_label']} · 半径 {result['radius_m']}m · "
                f"{len(result['candidates'])} 个候选 · {result['generated_at']}"
            ),
        )
        .replace("__MAP_SECTION__", map_section)
        .replace("__BODY__", body)
        .replace("__MAP_SCRIPT__", map_script)
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(page, encoding="utf-8")


def emit(result, args):
    html_path = None
    if args.html:
        html_path = Path(args.html).expanduser().resolve()
        render_html(
            result,
            html_path,
            js_key=os.environ.get("AMAP_JS_KEY", "").strip()
            or os.environ.get("AMAP_JSAPI_KEY", "").strip(),
            security_code=os.environ.get("AMAP_JS_SECURITY_CODE", "").strip(),
        )
        result["report_path"] = str(html_path)

    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print(render_markdown(result, html_path))


def _selftest_transport(url: str) -> dict:
    parsed = urllib.parse.urlparse(url)
    params = dict(urllib.parse.parse_qsl(parsed.query))
    path = parsed.path

    if path.endswith("/place/text"):
        page = int(params.get("page", "1"))
        if page > 2:
            return {"status": "1", "count": "0", "pois": []}
        keyword = params.get("keywords", "")
        center = {
            "购物中心": (120.10, 30.10),
            "写字楼": (120.12, 30.12),
            "地铁站": (120.11, 30.11),
        }.get(keyword, (120.10, 30.10))
        pois = []
        for index in range(25):
            lng = center[0] + ((index % 5) - 2) * 0.002
            lat = center[1] + ((index // 5) - 2) * 0.002
            pois.append(
                {
                    "name": f"{keyword}{index + 1}",
                    "address": "测试路",
                    "adname": "测试区",
                    "location": f"{lng:.6f},{lat:.6f}",
                }
            )
        return {"status": "1", "count": str(len(pois)), "pois": pois}

    if path.endswith("/place/around"):
        types = params.get("types", "")
        location = params.get("location", "")
        keywords = params.get("keywords", "")
        seed = sum(ord(char) for char in f"{types}{location}{keywords}")
        count = 3 + seed % 40
        pois = [
            {"name": f"样例{index + 1}", "distance": str(100 + index * 30)}
            for index in range(min(5, count))
        ]
        return {"status": "1", "count": str(count), "pois": pois}

    if path.endswith("/geocode/geo"):
        address = params.get("address", "")
        seed = sum(ord(char) for char in address)
        lng = 120.10 + (seed % 20) * 0.005
        lat = 30.10 + (seed % 17) * 0.005
        return {
            "status": "1",
            "geocodes": [
                {
                    "location": f"{lng:.6f},{lat:.6f}",
                    "formatted_address": address,
                    "district": "测试区",
                }
            ],
        }

    if path.endswith("/traffic/status/circle"):
        return {"status": "1", "trafficinfo": {"evaluation": {"status": "畅通"}}}

    return {"status": "1"}


def run_selftest() -> int:
    global HTTP_GET
    original = HTTP_GET
    HTTP_GET = _selftest_transport
    checks = []

    def check(name, condition):
        checks.append((name, bool(condition)))
        print(("PASS " if condition else "FAIL ") + name)

    try:
        client = Client("selftest", delay=0)
        anchors = search_anchors(client, "测试市", 2)
        check("锚点检索返回 POI", len(anchors) >= 30)

        clusters = cluster_anchors(anchors, 5)
        check("候选区位聚类", 1 <= len(clusters) <= 5)

        profile_key, profile = resolve_profile("咖啡")
        check("业态解析", profile_key == "coffee")

        results = [analyze(client, cluster, profile, 800) for cluster in clusters]
        check(
            "六类 POI 均已统计",
            all(set(item["evidence"]["poi_counts"]) == set(CATEGORY_TYPES) for item in results),
        )

        ranked = rank_candidates(results, "测试市", "咖啡", profile_key, profile, 800, "discover")
        totals = [item["scores"]["total"] for item in ranked["candidates"]]
        check("综合分在 0-100", all(0 <= total <= 100 for total in totals))
        check("按综合分降序", totals == sorted(totals, reverse=True))

        candidates = [geocode(client, "测试地址A", "测试市"), geocode(client, "测试地址B", "测试市")]
        scored = rank_candidates(
            [analyze(client, candidate, profile, 800) for candidate in candidates],
            "测试市",
            "咖啡",
            profile_key,
            profile,
            800,
            "score",
        )
        check("候选对比返回 2 项", len(scored["candidates"]) == 2)
        check("对比模式排名连续", [item["rank"] for item in scored["candidates"]] == [1, 2])

        cluster_config = {"mode": "cluster", "ideal": 10, "saturation": 30}
        check(
            "竞争度在理想值最高",
            competition_score(10, cluster_config) > competition_score(30, cluster_config)
            and competition_score(10, cluster_config) > competition_score(0, cluster_config),
        )

        generic_key, generic_profile = resolve_profile("generic")
        generic = analyze(client, clusters[0], generic_profile, 800)
        check(
            "通用业态竞争度按中性处理",
            generic["evidence"]["competitors"] == 0 and generic["scores"]["competition"] == 60.0,
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            report_path = Path(temp_dir) / "report.html"
            render_html(ranked, report_path)
            content = report_path.read_text(encoding="utf-8")
            check(
                "HTML 报告生成",
                "区位推荐" in content and ranked["candidates"][0]["name"] in content,
            )
    finally:
        HTTP_GET = original

    failures = [name for name, ok in checks if not ok]
    print(f"selftest: {len(checks) - len(failures)}/{len(checks)} passed")
    return 0 if not failures else 1


def get_key() -> str:
    for name in KEY_ENV_NAMES:
        value = os.environ.get(name, "").strip()
        if value:
            return value
    raise SystemExit(
        "未找到高德 Web 服务 Key。请先设置环境变量 AMAP_KEY"
        '（PowerShell: $env:AMAP_KEY="..."），'
        "申请地址：https://lbs.amap.com/api/webservice/create-project-and-key"
    )


def add_common_arguments(parser, default_top: int):
    parser.add_argument("--business", default="coffee", help="业态：coffee/tea/restaurant/convenience/gym 或中文/自定义关键词")
    parser.add_argument("--radius", type=int, default=800, help="搜索半径（米），200-5000")
    parser.add_argument("--top", type=int, default=default_top, help="输出前 N 个结果，0 表示全部")
    parser.add_argument("--cache-dir", default="", help="API 响应缓存目录，重复分析同一区域时省配额")
    parser.add_argument("--json", action="store_true", help="输出 JSON")
    parser.add_argument("--html", default="", help="生成可视化 HTML 报告到指定路径")


def build_parser():
    parser = argparse.ArgumentParser(description="高德区位推荐与选址评估")
    parser.add_argument("--selftest", action="store_true", help="用合成数据跑离线自测")

    subparsers = parser.add_subparsers(dest="command")

    discover = subparsers.add_parser("discover", help="城市 + 业态，自动发现候选区位")
    discover.add_argument("--city", required=True, help="城市名，如 杭州")
    discover.add_argument("--max-candidates", type=int, default=8, help="参与打分的候选数上限")
    discover.add_argument("--anchor-pages", type=int, default=2, help="每类锚点检索页数，每页 25 条")
    add_common_arguments(discover, 5)

    score = subparsers.add_parser("score", help="对给定候选地址做对比打分")
    score.add_argument("--city", default="", help="城市名，用于提高地理编码准确率")
    score.add_argument("--candidates", required=True, help="候选地址，逗号分隔，至少 2 个")
    add_common_arguments(score, 0)

    return parser


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")

    parser = build_parser()
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()
    if not args.command:
        parser.print_help()
        return 2
    if not 200 <= args.radius <= 5000:
        parser.error("--radius 需在 200~5000 米之间")
    if args.command == "score" and len(split_candidates(args.candidates)) < 2:
        parser.error("--candidates 至少需要 2 个地址，用逗号分隔")

    cache_dir = None
    if args.cache_dir:
        cache_dir = Path(args.cache_dir).expanduser()
        cache_dir.mkdir(parents=True, exist_ok=True)

    client = Client(get_key(), cache_dir=cache_dir)
    try:
        if args.command == "discover":
            result = run_discover(args, client)
        else:
            result = run_score(args, client)
        if args.top and args.top > 0:
            result["candidates"] = result["candidates"][: args.top]
        result["warnings"] = client.warnings
        emit(result, args)
        return 0
    except AmapError as exc:
        print(f"高德 API 调用失败：{exc}", file=sys.stderr)
        print("排查：Key 是否启用 Web 服务、是否超出当日配额、地址是否在城市范围内。", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
