#!/usr/bin/env python3
import argparse
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


COORD_STEP_MINUTES = 0.0051
LAT_BITS = 22
LON_BITS = 23
LAT_MAX_MINUTES = 179 * 60 + 59.9949
LON_MAX_MINUTES = 359 * 60 + 59.9949


@dataclass
class Route:
  index: int
  extremities: List[Tuple[int, int]]
  report_time: Optional[Tuple[int, int, int, int]]
  critical_points: List[Tuple[int, int]]


def convert_coord(text: str, max_minutes: float, bits: int, label: str) -> int:
  m = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*度\s*(\d+(?:\.\d+)?)\s*分?\s*", text)
  if not m:
    raise ValueError(f"invalid {label}: {text!r}")
  degrees = float(m.group(1))
  minutes = float(m.group(2))
  total_minutes = degrees * 60.0 + minutes
  if total_minutes < 0 or total_minutes > max_minutes:
    max_degrees = int(max_minutes // 60)
    max_minute_part = max_minutes - max_degrees * 60
    raise ValueError(f"{label} out of range 0deg0min..{max_degrees}deg{max_minute_part:.4f}min: {text!r}")
  value = round(total_minutes / COORD_STEP_MINUTES)
  max_value = (1 << bits) - 1
  if value > max_value:
    raise ValueError(f"{label} exceeds {bits}-bit field after conversion: {text!r} -> {value}")
  return value


def convert_latitude(text: str) -> int:
  return convert_coord(text, LAT_MAX_MINUTES, LAT_BITS, "latitude")


def convert_longitude(text: str) -> int:
  return convert_coord(text, LON_MAX_MINUTES, LON_BITS, "longitude")


def parse_points(text: str) -> List[Tuple[int, int]]:
  points: List[Tuple[int, int]] = []
  pattern = re.compile(r"\[\s*([^\[\],，]+?)\s*[,，]\s*([^\[\],，]+?)\s*\]")
  for m in pattern.finditer(text):
    points.append((convert_latitude(m.group(1)), convert_longitude(m.group(2))))
  return points


def parse_report_time(text: str) -> Optional[Tuple[int, int, int, int]]:
  if not text.strip():
    return None
  m = re.search(r"(\d+)\s*月\s*(\d+)\s*日\s*(\d+)\s*时\s*(\d+)\s*分", text)
  if not m:
    raise ValueError(f"invalid report time: {text.strip()!r}")
  month, day, hour, minute = (int(m.group(i)) for i in range(1, 5))
  if not (1 <= month <= 12):
    raise ValueError(f"month out of range: {month}")
  if not (1 <= day <= 31):
    raise ValueError(f"day out of range: {day}")
  if not (0 <= hour <= 23):
    raise ValueError(f"hour out of range: {hour}")
  if not (0 <= minute <= 59):
    raise ValueError(f"minute out of range: {minute}")
  return month, day, hour, minute


def split_route_sections(body: str) -> Dict[str, str]:
  matches = list(re.finditer(r"(途经点|报告时间|关键点)\s*[:：]?", body))
  sections: Dict[str, str] = {}
  for i, m in enumerate(matches):
    start = m.end()
    end = matches[i + 1].start() if i + 1 < len(matches) else len(body)
    sections[m.group(1)] = body[start:end].strip()
  return sections


def parse_routes(text: str) -> List[Route]:
  headers = list(re.finditer(r"^\s*路径\s*(\d+)\b.*$", text, flags=re.M))
  if not headers:
    raise ValueError("no route blocks found; expected keyword like '路径1'")

  routes: List[Route] = []
  for i, header in enumerate(headers):
    start = header.end()
    end = headers[i + 1].start() if i + 1 < len(headers) else len(text)
    route_index = int(header.group(1))
    sections = split_route_sections(text[start:end])
    extremities = parse_points(sections.get("途经点", ""))
    if not extremities:
      raise ValueError(f"路径{route_index} missing 途经点 points")
    routes.append(Route(
      index=route_index,
      extremities=extremities,
      report_time=parse_report_time(sections.get("报告时间", "")),
      critical_points=parse_points(sections.get("关键点", "")),
    ))
  return routes


def sub(parent: ET.Element, tag: str, attrs: Optional[Dict[str, str]] = None, text: Optional[int] = None) -> ET.Element:
  elem = ET.SubElement(parent, tag, attrs or {})
  if text is not None:
    elem.text = str(text)
  return elem


def data_unit(parent: ET.Element, dfi: str, dui: str, value: int) -> ET.Element:
  return sub(parent, "DataUnit", {"DFI": dfi, "DUI": dui}, value)


def indicator(parent: ET.Element, tag: str, dfi: str, dui: str, value: int) -> ET.Element:
  return sub(parent, tag, {"DFI": dfi, "DUI": dui}, value)


def append_point(parent: ET.Element, lat: int, lon: int) -> None:
  data_unit(parent, "281", "014", lat)
  data_unit(parent, "282", "014", lon)


def build_message(routes: List[Route], args: argparse.Namespace) -> ET.Element:
  root = ET.Element("MessageContent", {"message": "Land Route"})
  header = sub(root, "Header")
  for name, value in [
    ("version", args.version),
    ("length", 0),
    ("messageId", args.message_id),
    ("originator", args.originator),
    ("destination", args.destination),
  ]:
    sub(header, "Field", {"name": name}, value)

  body = sub(root, "Body")
  for route_i, route in enumerate(routes):
    route_elem = sub(body, "Group", {"name": "MultipleRoute"})
    indicator(route_elem, "GRI", "4045", "001", 0 if route_i + 1 == len(routes) else 1)

    for point_i, (lat, lon) in enumerate(route.extremities):
      point_elem = sub(route_elem, "Group", {"name": "RouteExtremeties"})
      indicator(point_elem, "GRI", "4045", "001", 0 if point_i + 1 == len(route.extremities) else 1)
      append_point(point_elem, lat, lon)

    report_elem = sub(route_elem, "Group", {"name": "ReportTime"})
    indicator(report_elem, "GPI", "4014", "001", 1 if route.report_time else 0)
    if route.report_time:
      month, day, hour, minute = route.report_time
      data_unit(report_elem, "4099", "001", month)
      data_unit(report_elem, "4019", "001", day)
      data_unit(report_elem, "792", "001", hour)
      data_unit(report_elem, "797", "004", minute)

    route_data_elem = sub(route_elem, "Group", {"name": "RouteData"})
    indicator(route_data_elem, "GPI", "4014", "001", 1 if route.critical_points else 0)
    for point_i, (lat, lon) in enumerate(route.critical_points):
      critical_elem = sub(route_data_elem, "Group", {"name": "CriticalPoints"})
      indicator(critical_elem, "GRI", "4045", "001", 0 if point_i + 1 == len(route.critical_points) else 1)
      append_point(critical_elem, lat, lon)
  return root


def main() -> int:
  parser = argparse.ArgumentParser(description="Convert Chinese route description text into VMF MessageContent XML.")
  parser.add_argument("input", nargs="?", default="路线描述.txt")
  parser.add_argument("-o", "--output", default="route_message.xml")
  parser.add_argument("--version", type=int, default=1)
  parser.add_argument("--message-id", type=int, default=0)
  parser.add_argument("--originator", type=int, default=0)
  parser.add_argument("--destination", type=int, default=0)
  args = parser.parse_args()

  text = Path(args.input).read_text(encoding="utf-8")
  routes = parse_routes(text)
  root = build_message(routes, args)
  tree = ET.ElementTree(root)
  ET.indent(tree, space="  ")
  tree.write(args.output, encoding="utf-8", xml_declaration=True)
  print(f"wrote {args.output}: routes={len(routes)}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
