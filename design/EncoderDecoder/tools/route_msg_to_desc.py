#!/usr/bin/env python3
import argparse
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path
from typing import List, Optional, Tuple


COORD_STEP_MINUTES = Decimal("0.0051")
LAT_BITS = 22
LON_BITS = 23
LAT_MAX_MINUTES = Decimal(179 * 60) + Decimal("59.9949")
LON_MAX_MINUTES = Decimal(359 * 60) + Decimal("59.9949")


@dataclass
class Route:
  extremities: List[Tuple[int, int]]
  report_time: Optional[Tuple[int, int, int, int]]
  critical_points: List[Tuple[int, int]]


def children_named(parent: ET.Element, tag: str, name: str) -> List[ET.Element]:
  return [child for child in list(parent) if child.tag == tag and child.get("name") == name]


def first_child_named(parent: ET.Element, tag: str, name: str) -> Optional[ET.Element]:
  matches = children_named(parent, tag, name)
  return matches[0] if matches else None


def find_data_unit(parent: ET.Element, dfi: str, dui: str) -> ET.Element:
  for child in list(parent):
    if child.tag == "DataUnit" and child.get("DFI") == dfi and child.get("DUI") == dui:
      return child
  raise ValueError(f"missing DataUnit DFI={dfi} DUI={dui} under {parent.get('name', parent.tag)!r}")


def find_indicator(parent: ET.Element, tag: str, default: int = 0) -> int:
  for child in list(parent):
    if child.tag == tag:
      return parse_int(child)
  return default


def parse_int(elem: ET.Element) -> int:
  text = (elem.text or "").strip()
  if not text:
    raise ValueError(f"missing integer text in <{elem.tag}>")
  return int(text)


def parse_point(parent: ET.Element) -> Tuple[int, int]:
  lat = parse_int(find_data_unit(parent, "281", "014"))
  lon = parse_int(find_data_unit(parent, "282", "014"))
  return lat, lon


def parse_report_time(route_elem: ET.Element) -> Optional[Tuple[int, int, int, int]]:
  report_elem = first_child_named(route_elem, "Group", "ReportTime")
  if report_elem is None or find_indicator(report_elem, "GPI", 0) == 0:
    return None
  month = parse_int(find_data_unit(report_elem, "4099", "001"))
  day = parse_int(find_data_unit(report_elem, "4019", "001"))
  hour = parse_int(find_data_unit(report_elem, "792", "001"))
  minute = parse_int(find_data_unit(report_elem, "797", "004"))
  return month, day, hour, minute


def parse_critical_points(route_elem: ET.Element) -> List[Tuple[int, int]]:
  route_data = first_child_named(route_elem, "Group", "RouteData")
  if route_data is None or find_indicator(route_data, "GPI", 0) == 0:
    return []
  return [parse_point(group) for group in children_named(route_data, "Group", "CriticalPoints")]


def parse_routes(root: ET.Element) -> List[Route]:
  body = root.find("Body")
  if body is None:
    raise ValueError("missing Body")
  routes: List[Route] = []
  for route_elem in children_named(body, "Group", "MultipleRoute"):
    extremities = [parse_point(group) for group in children_named(route_elem, "Group", "RouteExtremeties")]
    if not extremities:
      raise ValueError("MultipleRoute missing RouteExtremeties")
    routes.append(Route(
      extremities=extremities,
      report_time=parse_report_time(route_elem),
      critical_points=parse_critical_points(route_elem),
    ))
  if not routes:
    raise ValueError("missing MultipleRoute groups")
  return routes


def validate_coord_value(value: int, bits: int, max_minutes: Decimal, label: str) -> None:
  if value < 0:
    raise ValueError(f"{label} encoded value is negative: {value}")
  max_value = (1 << bits) - 1
  if value > max_value:
    raise ValueError(f"{label} encoded value exceeds {bits}-bit field: {value}")
  minutes = Decimal(value) * COORD_STEP_MINUTES
  if minutes > max_minutes:
    raise ValueError(f"{label} encoded value exceeds configured range: {value}")


def format_minutes(minutes: Decimal) -> str:
  rounded = minutes.quantize(Decimal("0.0001"), rounding=ROUND_HALF_UP)
  text = format(rounded, "f").rstrip("0").rstrip(".")
  return text if text else "0"


def format_coord(value: int, bits: int, max_minutes: Decimal, label: str) -> str:
  validate_coord_value(value, bits, max_minutes, label)
  total_minutes = Decimal(value) * COORD_STEP_MINUTES
  degrees = int(total_minutes // Decimal(60))
  minutes = total_minutes - Decimal(degrees * 60)
  return f"{degrees}度{format_minutes(minutes)}分"


def format_point(point: Tuple[int, int]) -> str:
  lat, lon = point
  lat_text = format_coord(lat, LAT_BITS, LAT_MAX_MINUTES, "latitude")
  lon_text = format_coord(lon, LON_BITS, LON_MAX_MINUTES, "longitude")
  return f"[{lat_text}，{lon_text}]"


def format_points(points: List[Tuple[int, int]]) -> str:
  return ",".join(format_point(point) for point in points)


def build_text(routes: List[Route]) -> str:
  blocks: List[str] = []
  for i, route in enumerate(routes, start=1):
    lines = [
      f"路径{i}",
      f"  途经点： {format_points(route.extremities)}",
    ]
    if route.report_time is not None:
      month, day, hour, minute = route.report_time
      lines.append(f"  报告时间： {month} 月 {day} 日 {hour} 时 {minute} 分")
    if route.critical_points:
      lines.append(f"  关键点： {format_points(route.critical_points)}")
    blocks.append("\n".join(lines))
  return "\n  \n".join(blocks) + "\n"


def main() -> int:
  parser = argparse.ArgumentParser(description="Convert Land Route MessageContent XML into Chinese route description text.")
  parser.add_argument("input", nargs="?", default="route_message.xml")
  parser.add_argument("-o", "--output", default="路线描述_out.txt")
  args = parser.parse_args()

  root = ET.parse(args.input).getroot()
  routes = parse_routes(root)
  Path(args.output).write_text(build_text(routes), encoding="utf-8")
  print(f"wrote {args.output}: routes={len(routes)}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
