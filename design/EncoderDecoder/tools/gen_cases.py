#!/usr/bin/env python3
import argparse
import json
import random
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


@dataclass
class CmdResult:
  code: int
  out: str
  err: str


@dataclass
class CmpClause:
  path: str
  op: str
  value: str
  source: str
  owner_id: str


@dataclass
class AllowedDomain:
  values: List[int] = field(default_factory=list)
  ranges: List[Tuple[int, int]] = field(default_factory=list)


@dataclass
class CoverageTarget:
  id: str
  kind: str
  path: str
  node_type: str
  value: Optional[int] = None
  count: Optional[int] = None


@dataclass
class BuildPlan:
  present_paths: Set[str] = field(default_factory=set)
  absent_paths: Set[str] = field(default_factory=set)
  repeat_counts: Dict[str, int] = field(default_factory=dict)
  forced_values: Dict[str, int] = field(default_factory=dict)


@dataclass
class AcceptanceResult:
  ok: bool
  repairs: List[str]
  validate_attempts: int
  last_error: str


def run_cmd(args: List[str]) -> CmdResult:
  p = subprocess.run(args, capture_output=True, text=True)
  return CmdResult(p.returncode, p.stdout, p.stderr)


def parse_bool(v: Optional[str]) -> bool:
  if v is None:
    return False
  return v.strip().lower() in {"1", "true"}


def bits_max(bits: int) -> int:
  if bits <= 0:
    raise ValueError(f"invalid bits: {bits}")
  if bits >= 64:
    return (1 << 64) - 1
  return (1 << bits) - 1


def require_bits(elem: ET.Element, ctx: str) -> int:
  v = elem.attrib.get("bits")
  if v is None:
    raise ValueError(f"missing bits: {ctx}")
  b = int(v)
  if b <= 0 or b > 64:
    raise ValueError(f"invalid bits {b}: {ctx}")
  return b


def find_named_children(parent: ET.Element, tag: str, name: str) -> List[ET.Element]:
  return [c for c in list(parent) if c.tag == tag and c.attrib.get("name") == name]


def split_path(path: str) -> List[str]:
  out: List[str] = []
  cur: List[str] = []
  depth = 0
  for ch in path:
    if ch == "(":
      depth += 1
    elif ch == ")":
      depth -= 1
    if ch == "." and depth == 0:
      out.append("".join(cur))
      cur = []
    else:
      cur.append(ch)
  if cur:
    out.append("".join(cur))
  return out


SEG_RE = re.compile(r"^([GFDI])\((.+)\)(?:\[(\d+)\])?$")


def parse_segment(seg: str) -> Tuple[str, str, Optional[int]]:
  m = SEG_RE.match(seg)
  if not m:
    raise ValueError(f"invalid segment: {seg}")
  kind, name, idx = m.group(1), m.group(2), m.group(3)
  return kind, name, int(idx) if idx is not None else None


def path_join(base: str, segment: str) -> str:
  return f"{base}.{segment}" if base else segment


def parent_path(path: str) -> Optional[str]:
  parts = split_path(path)
  if len(parts) <= 1:
    return None
  return ".".join(parts[:-1])


def expand_alias(path: str, aliases: Dict[str, str], depth: int = 0) -> str:
  if not path.startswith("$"):
    return path
  if depth > 8:
    raise ValueError(f"alias too deep: {path}")
  dot = path.find(".")
  key = path[1:] if dot < 0 else path[1:dot]
  if key not in aliases:
    raise ValueError(f"unknown alias: {key}")
  suffix = "" if dot < 0 else path[dot:]
  return expand_alias(aliases[key] + suffix, aliases, depth + 1)


def resolve_message_path(msg_root: ET.Element, raw_path: str, aliases: Dict[str, str]) -> ET.Element:
  path = expand_alias(raw_path, aliases)
  parts = split_path(path)
  if not parts:
    raise ValueError("empty path")
  idx = 0
  if parts[0] == "H":
    cur = msg_root.find("Header")
    idx = 1
  elif parts[0] == "B":
    cur = msg_root.find("Body")
    idx = 1
  else:
    raise ValueError(f"path must start with H/B: {raw_path}")
  if cur is None:
    raise ValueError(f"missing root for {raw_path}")

  for i in range(idx, len(parts)):
    kind, name, index = parse_segment(parts[i])
    if kind in ("G", "F"):
      tag = "Group" if kind == "G" else "Field"
      matches = find_named_children(cur, tag, name)
      if not matches:
        raise ValueError(f"path not found: {raw_path}")
      if index is None:
        if len(matches) != 1:
          raise ValueError(f"ambiguous path: {raw_path}")
        cur = matches[0]
      else:
        if index >= len(matches):
          raise ValueError(f"index out of range: {raw_path}")
        cur = matches[index]
    elif kind == "D":
      if i + 1 != len(parts):
        raise ValueError(f"D must be leaf: {raw_path}")
      if parts[0] == "H" and cur.tag == "Header":
        matches = find_named_children(cur, "Field", name)
      else:
        matches = find_named_children(cur, "DataUnit", name)
      if not matches:
        raise ValueError(f"leaf not found: {raw_path}")
      if index is None:
        if len(matches) != 1:
          raise ValueError(f"ambiguous leaf: {raw_path}")
        return matches[0]
      if index >= len(matches):
        raise ValueError(f"index out of range: {raw_path}")
      return matches[index]
    elif kind == "I":
      if i + 1 != len(parts):
        raise ValueError(f"I must be leaf: {raw_path}")
      ind = cur.find(name)
      if ind is None:
        raise ValueError(f"indicator not found: {raw_path}")
      return ind
  raise ValueError(f"path is not leaf: {raw_path}")


def resolve_dict_bits(dict_root: ET.Element, raw_path: str, aliases: Dict[str, str]) -> int:
  path = expand_alias(raw_path, aliases)
  parts = split_path(path)
  if not parts:
    raise ValueError("empty path")
  idx = 0
  if parts[0] == "H":
    cur = dict_root.find("Header")
    idx = 1
  elif parts[0] == "B":
    cur = dict_root.find("Body")
    idx = 1
  else:
    raise ValueError(f"path must start H/B: {raw_path}")
  if cur is None:
    raise ValueError(f"missing dict root for {raw_path}")

  for i in range(idx, len(parts)):
    kind, name, _ = parse_segment(parts[i])
    if kind in ("G", "F"):
      tag = "Group" if kind == "G" else "Field"
      matches = [c for c in list(cur) if c.tag == tag and c.attrib.get("name") == name]
      if not matches:
        raise ValueError(f"dict path not found: {raw_path}")
      cur = matches[0]
    elif kind == "D":
      if i + 1 != len(parts):
        raise ValueError(f"dict D not leaf: {raw_path}")
      if parts[0] == "H" and cur.tag == "Header":
        matches = [c for c in list(cur) if c.tag == "Field" and c.attrib.get("name") == name]
      else:
        matches = [c for c in list(cur) if c.tag == "DataUnit" and c.attrib.get("name") == name]
      if not matches:
        raise ValueError(f"dict leaf not found: {raw_path}")
      return require_bits(matches[0], raw_path)
    elif kind == "I":
      if i + 1 != len(parts):
        raise ValueError(f"dict I not leaf: {raw_path}")
      ind = next((c for c in list(cur) if c.tag == name), None)
      if ind is None:
        raise ValueError(f"dict indicator not found: {raw_path}")
      return require_bits(ind, raw_path)
  raise ValueError(f"path is not leaf: {raw_path}")


def build_aliases(dict_root: ET.Element) -> Dict[str, str]:
  aliases: Dict[str, str] = {}
  rules = dict_root.find("Rules")
  if rules is None:
    return aliases
  aliases_node = rules.find("Aliases")
  if aliases_node is None:
    return aliases
  for node in list(aliases_node):
    if node.tag != "Alias":
      continue
    name = node.attrib.get("name")
    path = node.attrib.get("path")
    if name and path:
      aliases[name] = path
  return aliases


def parse_uint_str(s: str) -> int:
  return int(s.strip(), 0)


def parse_uint_list(s: str) -> List[int]:
  vals: List[int] = []
  for token in s.split(","):
    t = token.strip()
    if t:
      vals.append(parse_uint_str(t))
  return vals


def parse_content_domains(content_root: Optional[ET.Element]) -> Dict[Tuple[str, str], AllowedDomain]:
  domains: Dict[Tuple[str, str], AllowedDomain] = {}
  if content_root is None:
    return domains
  for dfi in content_root.findall("DFI"):
    dfi_num = dfi.attrib.get("num")
    if not dfi_num:
      continue
    for dui in dfi.findall("DUI"):
      dui_num = dui.attrib.get("num")
      if not dui_num:
        continue
      domain = AllowedDomain()
      allow = dui.find("Constraints/AllowList")
      if allow is not None:
        for node in list(allow):
          if node.tag == "Value":
            v = node.attrib.get("v")
            if v is not None:
              domain.values.append(parse_uint_str(v))
          elif node.tag == "Range":
            a = node.attrib.get("from")
            b = node.attrib.get("to")
            if a is not None and b is not None:
              domain.ranges.append((parse_uint_str(a), parse_uint_str(b)))
      domains[(dfi_num, dui_num)] = domain
  return domains


def find_first_expr_node(parent: Optional[ET.Element]) -> Optional[ET.Element]:
  if parent is None:
    return None
  for child in list(parent):
    if child.tag in {"All", "Any", "Not", "Cmp"}:
      return child
  return None


def collect_cmp_in_expr(expr: Optional[ET.Element]) -> List[ET.Element]:
  if expr is None:
    return []
  return [n for n in expr.iter() if n.tag == "Cmp"]


def build_rule_models(dict_root: ET.Element) -> Tuple[List[CmpClause], Dict[str, List[CmpClause]]]:
  case_clauses: List[CmpClause] = []
  cond_then_clauses: Dict[str, List[CmpClause]] = {}
  rules = dict_root.find("Rules")
  if rules is None:
    return case_clauses, cond_then_clauses

  case_rules = rules.find("CaseRules")
  if case_rules is not None:
    for case in list(case_rules):
      if case.tag != "Case":
        continue
      cid = case.attrib.get("id", "NO_CASE_ID")
      expr = find_first_expr_node(case)
      for cmp_node in collect_cmp_in_expr(expr):
        p = cmp_node.attrib.get("path")
        op = cmp_node.attrib.get("op")
        val = cmp_node.attrib.get("value")
        if p and op and val is not None:
          case_clauses.append(CmpClause(path=p, op=op, value=val, source="case", owner_id=cid))

  cond_rules = rules.find("ConditionRules")
  if cond_rules is not None:
    for cond in list(cond_rules):
      if cond.tag != "Condition":
        continue
      cid = cond.attrib.get("id", "NO_CONDITION_ID")
      then_expr = find_first_expr_node(cond.find("Then"))
      clauses: List[CmpClause] = []
      for cmp_node in collect_cmp_in_expr(then_expr):
        p = cmp_node.attrib.get("path")
        op = cmp_node.attrib.get("op")
        val = cmp_node.attrib.get("value")
        if p and op and val is not None:
          clauses.append(CmpClause(path=p, op=op, value=val, source="condition_then", owner_id=cid))
      if clauses:
        cond_then_clauses[cid] = clauses

  return case_clauses, cond_then_clauses


def choose_value_to_satisfy(op: str, value_text: str, bits: int, old: int, rng: random.Random) -> Optional[int]:
  max_v = bits_max(bits)
  op_s = op.strip().lower()
  if op_s == "eq":
    v = parse_uint_str(value_text)
    return v if 0 <= v <= max_v else None
  if op_s == "ne":
    base = parse_uint_str(value_text)
    for cand in [old, base + 1, 0, 1, max_v]:
      if 0 <= cand <= max_v and cand != base:
        return cand
    return None
  if op_s == "gt":
    base = parse_uint_str(value_text)
    return base + 1 if base + 1 <= max_v else None
  if op_s == "ge":
    base = parse_uint_str(value_text)
    return base if 0 <= base <= max_v else None
  if op_s == "lt":
    base = parse_uint_str(value_text)
    return base - 1 if base > 0 else None
  if op_s == "le":
    base = parse_uint_str(value_text)
    return base if 0 <= base <= max_v else None
  if op_s == "in":
    for v in parse_uint_list(value_text):
      if 0 <= v <= max_v:
        return v
    return None
  if op_s == "not_in":
    ban = set(parse_uint_list(value_text))
    for cand in [old, 0, 1, max_v, max_v // 2]:
      if 0 <= cand <= max_v and cand not in ban:
        return cand
    for _ in range(16):
      cand = rng.randint(0, max_v)
      if cand not in ban:
        return cand
    return None
  return None


def text_to_int(elem: ET.Element) -> int:
  return int((elem.text or "0").strip(), 0)


def apply_cmp_clause(msg_root: ET.Element, dict_root: ET.Element, aliases: Dict[str, str], clause: CmpClause,
                     rng: random.Random) -> bool:
  try:
    leaf = resolve_message_path(msg_root, clause.path, aliases)
    bits = resolve_dict_bits(dict_root, clause.path, aliases)
    old = text_to_int(leaf)
    new_value = choose_value_to_satisfy(clause.op, clause.value, bits, old, rng)
    if new_value is None:
      return False
    leaf.text = str(new_value)
    return True
  except Exception:
    return False


def parse_condition_fail_ids(validate_output: str) -> List[str]:
  ids: List[str] = []
  for line in validate_output.splitlines():
    m = re.search(r"Condition failed:\s*(.+)$", line.strip())
    if m:
      ids.append(m.group(1).strip())
  return ids


def gen_numeric_value(bits: int, rng: random.Random) -> int:
  max_v = bits_max(bits)
  if max_v <= 1:
    return rng.randint(0, max_v)
  if rng.random() < 0.8:
    return rng.randint(0, min(max_v, 1024))
  return rng.randint(0, max_v)


def choose_domain_value(bits: int, domain: Optional[AllowedDomain], rng: random.Random) -> int:
  if domain is None or (not domain.values and not domain.ranges):
    return gen_numeric_value(bits, rng)
  choices: List[Tuple[str, int, int]] = []
  for value in domain.values:
    choices.append(("value", value, value))
  for a, b in domain.ranges:
    choices.append(("range", a, b))
  kind, a, b = rng.choice(choices)
  if kind == "value":
    return a
  return rng.randint(a, b)


def collect_value_boundary_values(bits: int, domain: Optional[AllowedDomain]) -> List[int]:
  values: List[int] = []

  def add(v: int) -> None:
    if 0 <= v <= bits_max(bits) and v not in values:
      values.append(v)

  if domain is not None and (domain.values or domain.ranges):
    for v in sorted(domain.values):
      add(v)
    if domain.ranges:
      add(min(a for a, _ in domain.ranges))
      add(max(b for _, b in domain.ranges))
  else:
    add(0)
    add(1)
    add(bits_max(bits))

  if not values:
    add(0)
  return values


def write_xml_with_header_comment(root: ET.Element, path: Path, header_comment: str) -> None:
  ET.indent(ET.ElementTree(root), space="  ")
  body = ET.tostring(root, encoding="unicode")
  path.parent.mkdir(parents=True, exist_ok=True)
  with path.open("w", encoding="utf-8") as f:
    f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
    f.write(f"<!-- {header_comment} -->\n")
    f.write(body)
    f.write("\n")


def build_target_plan(target: CoverageTarget) -> BuildPlan:
  plan = BuildPlan()
  parent = parent_path(target.path)
  if target.kind == "PATH_PRESENT":
    if parent is not None:
      plan.present_paths.add(parent)
  elif target.kind == "SELECTABLE_PRESENT":
    plan.present_paths.add(target.path)
  elif target.kind == "SELECTABLE_ABSENT":
    if parent is not None:
      plan.present_paths.add(parent)
    plan.absent_paths.add(target.path)
  elif target.kind == "REPEAT_COUNT":
    if parent is not None:
      plan.present_paths.add(parent)
    if target.count is not None:
      plan.repeat_counts[target.path] = target.count
    plan.present_paths.add(target.path)
  elif target.kind == "VALUE_BOUNDARY":
    if parent is not None:
      plan.present_paths.add(parent)
    if target.value is not None:
      plan.forced_values[target.path] = target.value
  return plan


def plan_requires_path(plan: BuildPlan, current_path: str) -> bool:
  if current_path in plan.present_paths:
    return True
  for path in plan.present_paths:
    if path.startswith(current_path + "."):
      return True
  for path in plan.repeat_counts:
    if path == current_path or path.startswith(current_path + "."):
      return True
  for path in plan.forced_values:
    if path.startswith(current_path + "."):
      return True
  return False


def append_indicator(parent: ET.Element, tag: str, value: int, attrs: Optional[Dict[str, str]] = None) -> ET.Element:
  e = ET.SubElement(parent, tag, attrs or {})
  e.text = str(value)
  return e


def build_message(dict_root: ET.Element, content_domains: Dict[Tuple[str, str], AllowedDomain], plan: BuildPlan,
                  rng: random.Random, max_repeat_default: int) -> ET.Element:
  root = ET.Element("MessageContent")
  msg_name = dict_root.attrib.get("name")
  if msg_name:
    root.set("message", msg_name)

  def add_children(dict_parent: ET.Element, msg_parent: ET.Element, current_path: str, in_header: bool = False) -> None:
    for child in list(dict_parent):
      if child.tag == "DataUnit":
        name = child.attrib.get("name")
        if not name:
          continue
        path = path_join(current_path, f"D({name})")
        bits = require_bits(child, path)
        out = ET.SubElement(msg_parent, "DataUnit", {
          "DFI": child.attrib.get("DFI", ""),
          "DUI": child.attrib.get("DUI", ""),
          "name": name,
        })
        if path in plan.forced_values:
          out.text = str(plan.forced_values[path])
        else:
          key = (child.attrib.get("DFI", ""), child.attrib.get("DUI", ""))
          out.text = str(choose_domain_value(bits, content_domains.get(key), rng))
      elif child.tag == "Field":
        name = child.attrib.get("name")
        if not name:
          continue
        if in_header:
          path = path_join(current_path, f"D({name})")
          bits = require_bits(child, path)
          out = ET.SubElement(msg_parent, "Field", {"name": name})
          if path in plan.forced_values:
            out.text = str(plan.forced_values[path])
          elif name == "length":
            out.text = "0"
          else:
            out.text = str(gen_numeric_value(bits, rng))
        else:
          add_container(child, msg_parent, "Field", path_join(current_path, f"F({name})"))
      elif child.tag == "Group":
        name = child.attrib.get("name")
        if name:
          add_container(child, msg_parent, "Group", path_join(current_path, f"G({name})"))

  def add_container(dict_container: ET.Element, msg_parent: ET.Element, tag: str, current_path: str) -> None:
    name = dict_container.attrib.get("name")
    if not name:
      return
    repeatable = parse_bool(dict_container.attrib.get("repeatable"))
    selectable = parse_bool(dict_container.attrib.get("selectable"))
    repeat_indicator = "GRI" if tag == "Group" else "FRI"
    select_indicator = "GPI" if tag == "Group" else "FPI"

    if repeatable:
      max_repeat = int(dict_container.attrib.get("max", max_repeat_default))
      instances = plan.repeat_counts.get(current_path, rng.randint(1, max_repeat))
    else:
      instances = 1

    requires_here = plan_requires_path(plan, current_path)
    for i in range(instances):
      inst = ET.SubElement(msg_parent, tag, {"name": name})
      present = 1
      if selectable:
        if current_path in plan.absent_paths:
          present = 0
        elif requires_here:
          present = 1
        else:
          present = rng.randint(0, 1)
        select_attrs = {}
        select_dfi = dict_container.find(select_indicator).attrib.get("DFI") if dict_container.find(select_indicator) is not None else None
        select_dui = dict_container.find(select_indicator).attrib.get("DUI") if dict_container.find(select_indicator) is not None else None
        if select_dfi and select_dui:
          select_attrs = {"DFI": select_dfi, "DUI": select_dui}
        append_indicator(inst, select_indicator, present, select_attrs)

      if present != 0:
        for sub in list(dict_container):
          if selectable and sub.tag == select_indicator:
            continue
          if repeatable and sub.tag == repeat_indicator:
            repeat_attrs = {}
            repeat_dfi = sub.attrib.get("DFI")
            repeat_dui = sub.attrib.get("DUI")
            if repeat_dfi and repeat_dui:
              repeat_attrs = {"DFI": repeat_dfi, "DUI": repeat_dui}
            append_indicator(inst, repeat_indicator, 0 if i + 1 == instances else 1, repeat_attrs)
            continue
          if sub.tag in {"GRI", "FRI", "GPI", "FPI"}:
            bits = require_bits(sub, path_join(current_path, sub.tag))
            ind_attrs = {}
            ind_dfi = sub.attrib.get("DFI")
            ind_dui = sub.attrib.get("DUI")
            if ind_dfi and ind_dui:
              ind_attrs = {"DFI": ind_dfi, "DUI": ind_dui}
            append_indicator(inst, sub.tag, rng.randint(0, min(bits_max(bits), 1)), ind_attrs)
            continue
          if sub.tag == "DataUnit":
            sub_name = sub.attrib.get("name")
            if not sub_name:
              continue
            path = path_join(current_path, f"D({sub_name})")
            bits = require_bits(sub, path)
            out = ET.SubElement(inst, "DataUnit", {
              "DFI": sub.attrib.get("DFI", ""),
              "DUI": sub.attrib.get("DUI", ""),
              "name": sub_name,
            })
            if path in plan.forced_values:
              out.text = str(plan.forced_values[path])
            else:
              key = (sub.attrib.get("DFI", ""), sub.attrib.get("DUI", ""))
              out.text = str(choose_domain_value(bits, content_domains.get(key), rng))
          elif sub.tag == "Field":
            sub_name = sub.attrib.get("name")
            if sub_name:
              add_container(sub, inst, "Field", path_join(current_path, f"F({sub_name})"))
          elif sub.tag == "Group":
            sub_name = sub.attrib.get("name")
            if sub_name:
              add_container(sub, inst, "Group", path_join(current_path, f"G({sub_name})"))

  header = dict_root.find("Header")
  body = dict_root.find("Body")
  if header is None or body is None:
    raise ValueError("dictionary missing Header/Body")
  msg_header = ET.SubElement(root, "Header")
  add_children(header, msg_header, "H", in_header=True)
  msg_body = ET.SubElement(root, "Body")
  add_children(body, msg_body, "B", in_header=False)
  return root


def is_semantically_present(node: ET.Element) -> bool:
  ind = node.find("GPI")
  if ind is None:
    ind = node.find("FPI")
  if ind is None:
    return True
  return (ind.text or "0").strip() != "0"


def collect_present_paths(msg_root: ET.Element) -> Set[str]:
  out: Set[str] = set()
  header = msg_root.find("Header")
  if header is not None:
    for child in list(header):
      if child.tag == "Field":
        name = child.attrib.get("name")
        if name:
          out.add(f"H.D({name})")

  def walk(parent: ET.Element, base_path: str) -> None:
    for child in list(parent):
      if child.tag == "DataUnit":
        name = child.attrib.get("name")
        if name:
          out.add(path_join(base_path, f"D({name})"))
        continue
      if child.tag not in {"Group", "Field"}:
        continue
      name = child.attrib.get("name")
      if not name:
        continue
      seg = "G" if child.tag == "Group" else "F"
      current_path = path_join(base_path, f"{seg}({name})")
      if not is_semantically_present(child):
        continue
      out.add(current_path)
      walk(child, current_path)

  body = msg_root.find("Body")
  if body is not None:
    walk(body, "B")
  return out


def resolve_message_matches(msg_root: ET.Element, raw_path: str) -> List[ET.Element]:
  parts = split_path(raw_path)
  if not parts:
    return []
  if parts[0] == "H":
    roots = [msg_root.find("Header")] if msg_root.find("Header") is not None else []
  elif parts[0] == "B":
    roots = [msg_root.find("Body")] if msg_root.find("Body") is not None else []
  else:
    return []

  current = roots
  for i in range(1, len(parts)):
    kind, name, _ = parse_segment(parts[i])
    next_nodes: List[ET.Element] = []
    for node in current:
      if node is None:
        continue
      if kind in {"G", "F"}:
        tag = "Group" if kind == "G" else "Field"
        next_nodes.extend(find_named_children(node, tag, name))
      elif kind == "D":
        tag = "Field" if parts[0] == "H" and node.tag == "Header" else "DataUnit"
        next_nodes.extend(find_named_children(node, tag, name))
      elif kind == "I":
        ind = node.find(name)
        if ind is not None:
          next_nodes.append(ind)
    current = next_nodes
  return current


def target_is_covered(msg_root: ET.Element, target: CoverageTarget) -> bool:
  present_paths = collect_present_paths(msg_root)
  if target.kind == "PATH_PRESENT":
    return target.path in present_paths
  if target.kind == "SELECTABLE_PRESENT":
    return target.path in present_paths
  if target.kind == "SELECTABLE_ABSENT":
    for node in resolve_message_matches(msg_root, target.path):
      if not is_semantically_present(node):
        return True
    return False
  if target.kind == "REPEAT_COUNT":
    return len(resolve_message_matches(msg_root, target.path)) == target.count
  if target.kind == "VALUE_BOUNDARY":
    for leaf in resolve_message_matches(msg_root, target.path):
      if text_to_int(leaf) == target.value:
        return True
    return False
  return False


def collect_all_covered_targets(msg_root: ET.Element, targets: List[CoverageTarget]) -> List[str]:
  covered: List[str] = []
  for target in targets:
    if target_is_covered(msg_root, target):
      covered.append(target.id)
  return covered


def collect_targets(dict_root: ET.Element, content_domains: Dict[Tuple[str, str], AllowedDomain]) -> List[CoverageTarget]:
  targets: List[CoverageTarget] = []
  seen: Set[str] = set()

  def add(target: CoverageTarget) -> None:
    if target.id not in seen:
      seen.add(target.id)
      targets.append(target)

  def walk(parent: ET.Element, base_path: str, in_header: bool = False) -> None:
    for child in list(parent):
      if child.tag == "DataUnit":
        name = child.attrib.get("name")
        if not name:
          continue
        path = path_join(base_path, f"D({name})")
        add(CoverageTarget(id=f"PATH_PRESENT:{path}", kind="PATH_PRESENT", path=path, node_type="DataUnit"))
        bits = require_bits(child, path)
        key = (child.attrib.get("DFI", ""), child.attrib.get("DUI", ""))
        for value in collect_value_boundary_values(bits, content_domains.get(key)):
          add(CoverageTarget(
            id=f"VALUE_BOUNDARY:{path}:{value}",
            kind="VALUE_BOUNDARY",
            path=path,
            node_type="DataUnit",
            value=value,
          ))
        continue

      if child.tag != "Field" and child.tag != "Group":
        continue
      name = child.attrib.get("name")
      if not name:
        continue
      if in_header:
        path = path_join(base_path, f"D({name})")
        add(CoverageTarget(id=f"PATH_PRESENT:{path}", kind="PATH_PRESENT", path=path, node_type="HeaderField"))
        if name != "length":
          bits = require_bits(child, path)
          for value in collect_value_boundary_values(bits, None):
            add(CoverageTarget(
              id=f"VALUE_BOUNDARY:{path}:{value}",
              kind="VALUE_BOUNDARY",
              path=path,
              node_type="HeaderField",
              value=value,
            ))
        continue

      seg = "G" if child.tag == "Group" else "F"
      path = path_join(base_path, f"{seg}({name})")
      selectable = parse_bool(child.attrib.get("selectable"))
      repeatable = parse_bool(child.attrib.get("repeatable"))
      if selectable:
        add(CoverageTarget(id=f"SELECTABLE_PRESENT:{path}", kind="SELECTABLE_PRESENT", path=path, node_type=child.tag))
        add(CoverageTarget(id=f"SELECTABLE_ABSENT:{path}", kind="SELECTABLE_ABSENT", path=path, node_type=child.tag))
      elif not repeatable:
        add(CoverageTarget(id=f"PATH_PRESENT:{path}", kind="PATH_PRESENT", path=path, node_type=child.tag))

      if repeatable:
        max_repeat = int(child.attrib.get("max", "1"))
        counts = [1]
        if max_repeat >= 2:
          counts.append(2)
        if max_repeat not in counts:
          counts.append(max_repeat)
        for count in counts:
          add(CoverageTarget(
            id=f"REPEAT_COUNT:{path}:{count}",
            kind="REPEAT_COUNT",
            path=path,
            node_type=child.tag,
            count=count,
          ))
      walk(child, path, in_header=False)

  header = dict_root.find("Header")
  body = dict_root.find("Body")
  if header is None or body is None:
    raise ValueError("dictionary missing Header/Body")
  walk(header, "H", in_header=True)
  walk(body, "B", in_header=False)
  return sorted(targets, key=lambda item: item.id)


def validate_case(validate_bin: Path, dict_path: Path, content_dict_path: Optional[Path], case_path: Path) -> CmdResult:
  args = [str(validate_bin), str(dict_path)]
  if content_dict_path is not None:
    args.append(str(content_dict_path))
  args.append(str(case_path))
  return run_cmd(args)


def accept_positive(msg_root: ET.Element, bins: Dict[str, Path], dict_root: ET.Element, dict_path: Path,
                    content_dict_path: Optional[Path], aliases: Dict[str, str], case_clauses: List[CmpClause],
                    cond_then_clauses: Dict[str, List[CmpClause]], rng: random.Random) -> AcceptanceResult:
  if content_dict_path is None:
    raise ValueError("content_dict_path is required for positive acceptance because vmf_encode now needs dic_content.xml")
  repairs: List[str] = []
  last_error = ""
  for attempt in range(1, 9):
    with tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False, encoding="utf-8") as tmp:
      tmp_path = Path(tmp.name)
    try:
      write_xml_with_header_comment(msg_root, tmp_path, "TARGET: TEMP; COVERED: 0")
      v = validate_case(bins["validate"], dict_path, content_dict_path, tmp_path)
      if v.code == 0:
        e = run_cmd([str(bins["encode"]), str(dict_path), str(content_dict_path), str(tmp_path), str(tmp_path) + ".bin"])
        if e.code == 0:
          return AcceptanceResult(True, repairs, attempt, "")
        last_error = ((e.out or "") + (e.err or "")).strip()
        return AcceptanceResult(False, repairs, attempt, last_error)

      output = ((v.out or "") + (v.err or "")).strip()
      last_error = output
      fixed = False
      if "No Case matched in CaseRules" in output and case_clauses:
        clause = rng.choice(case_clauses)
        if apply_cmp_clause(msg_root, dict_root, aliases, clause, rng):
          repairs.append(f"case:{clause.path}:{clause.op}:{clause.value}")
          fixed = True
      for cid in parse_condition_fail_ids(output):
        clauses = cond_then_clauses.get(cid, [])
        if clauses:
          clause = rng.choice(clauses)
          if apply_cmp_clause(msg_root, dict_root, aliases, clause, rng):
            repairs.append(f"condition:{cid}:{clause.path}:{clause.op}:{clause.value}")
            fixed = True
      if not fixed:
        return AcceptanceResult(False, repairs, attempt, last_error)
    finally:
      try:
        tmp_path.unlink(missing_ok=True)
        Path(str(tmp_path) + ".bin").unlink(missing_ok=True)
      except Exception:
        pass
  return AcceptanceResult(False, repairs, 8, last_error)


def ensure_bins(build_dir: Path) -> Dict[str, Path]:
  bins = {
    "validate": build_dir / "vmf_validate",
    "encode": build_dir / "vmf_encode",
    "decode": build_dir / "vmf_decode",
    "compare": build_dir / "xml_compare",
  }
  for name, path in bins.items():
    if not path.exists():
      raise FileNotFoundError(f"missing binary {name}: {path}")
  return bins


def target_to_dict(target: CoverageTarget) -> Dict[str, object]:
  return {
    "id": target.id,
    "kind": target.kind,
    "path": target.path,
    "node_type": target.node_type,
    "value": target.value,
    "count": target.count,
  }


def main() -> int:
  ap = argparse.ArgumentParser(description="Generate explainable positive VMF cases from dictionary.")
  ap.add_argument("--dict", dest="dict_path", default="dic.xml")
  ap.add_argument("--content-dict", dest="content_dict_path", default=None)
  ap.add_argument("--build-dir", default="build")
  ap.add_argument("--out-dir", default="generated")
  ap.add_argument("--seed", type=int, default=20260324)
  ap.add_argument("--max-repeat", type=int, default=3)
  ap.add_argument("--max-cases", type=int, default=0)
  ap.add_argument("--targets", default="path,selectable,repeat,value")
  ap.add_argument("--coverage-report-only", action="store_true")
  args = ap.parse_args()

  rng = random.Random(args.seed)
  dict_path = Path(args.dict_path).resolve()
  content_dict_path = Path(args.content_dict_path).resolve() if args.content_dict_path else None
  build_dir = Path(args.build_dir).resolve()
  out_dir = Path(args.out_dir).resolve()

  dict_root = ET.parse(dict_path).getroot()
  if dict_root.tag != "Message":
    raise ValueError("dictionary root must be <Message>")

  content_root = None
  if content_dict_path is not None:
    content_root = ET.parse(content_dict_path).getroot()

  aliases = build_aliases(dict_root)
  case_clauses, cond_then_clauses = build_rule_models(dict_root)
  content_domains = parse_content_domains(content_root)
  selected_kinds = {token.strip() for token in args.targets.split(",") if token.strip()}
  kind_map = {
    "path": {"PATH_PRESENT"},
    "selectable": {"SELECTABLE_PRESENT", "SELECTABLE_ABSENT"},
    "repeat": {"REPEAT_COUNT"},
    "value": {"VALUE_BOUNDARY"},
  }
  allowed_kinds: Set[str] = set()
  for token in selected_kinds:
    allowed_kinds.update(kind_map.get(token, {token}))
  all_targets = [t for t in collect_targets(dict_root, content_domains) if t.kind in allowed_kinds]

  out_dir.mkdir(parents=True, exist_ok=True)
  pos_dir = out_dir / "positive"
  pos_dir.mkdir(parents=True, exist_ok=True)

  if args.coverage_report_only:
    report_path = out_dir / "coverage_report.json"
    with report_path.open("w", encoding="utf-8") as f:
      json.dump({
        "seed": args.seed,
        "dict": str(dict_path),
        "content_dict": str(content_dict_path) if content_dict_path else None,
        "targets_total": len(all_targets),
        "targets": [target_to_dict(t) for t in all_targets],
      }, f, ensure_ascii=False, indent=2)
    print(f"Coverage report written: {report_path}")
    return 0

  bins = ensure_bins(build_dir)
  cases: List[Dict[str, object]] = []
  met_targets: Set[str] = set()
  target_status: Dict[str, Dict[str, object]] = {
    t.id: {"target": target_to_dict(t), "met": False, "case_index": None, "last_error": ""}
    for t in all_targets
  }

  for target in all_targets:
    if target.id in met_targets:
      continue
    if args.max_cases > 0 and len(cases) >= args.max_cases:
      break

    last_error = ""
    produced = False
    for _ in range(48):
      plan = build_target_plan(target)
      msg_root = build_message(dict_root, content_domains, plan, rng, args.max_repeat)
      acceptance = accept_positive(
        msg_root, bins, dict_root, dict_path, content_dict_path, aliases, case_clauses, cond_then_clauses, rng
      )
      if not acceptance.ok:
        last_error = acceptance.last_error
        continue
      if not target_is_covered(msg_root, target):
        last_error = f"accepted sample does not cover primary target {target.id}"
        continue

      covered_targets = collect_all_covered_targets(msg_root, all_targets)
      case_index = len(cases)
      case_path = pos_dir / f"pos_{case_index:03d}.xml"
      write_xml_with_header_comment(msg_root, case_path, f"TARGET: {target.id}; COVERED: {len(covered_targets)}")
      case_meta = {
        "path": str(case_path),
        "category": "positive",
        "error_type": "NONE",
        "primary_target": target.id,
        "covered_targets": covered_targets,
        "forced_paths": sorted(plan.present_paths),
        "forced_values": {k: v for k, v in sorted(plan.forced_values.items())},
        "repeat_settings": plan.repeat_counts,
        "selectable_settings": {p: "absent" for p in sorted(plan.absent_paths)},
        "repair_actions": acceptance.repairs,
        "validate_attempts": acceptance.validate_attempts,
      }
      for path in sorted(plan.present_paths):
        if path not in case_meta["selectable_settings"] and any(t.path == path and t.kind == "SELECTABLE_PRESENT" for t in all_targets):
          case_meta["selectable_settings"][path] = "present"
      cases.append(case_meta)
      for covered in covered_targets:
        met_targets.add(covered)
        target_status[covered]["met"] = True
        if target_status[covered]["case_index"] is None:
          target_status[covered]["case_index"] = case_index
      produced = True
      break

    if not produced:
      target_status[target.id]["last_error"] = last_error

  unmet_targets = [entry for entry in target_status.values() if not entry["met"]]

  manifest_path = out_dir / "cases_manifest.json"
  with manifest_path.open("w", encoding="utf-8") as f:
    json.dump({
      "seed": args.seed,
      "dict": str(dict_path),
      "content_dict": str(content_dict_path) if content_dict_path else None,
      "targets_total": len(all_targets),
      "targets_met": len(all_targets) - len(unmet_targets),
      "unmet_targets": unmet_targets,
      "cases": cases,
    }, f, ensure_ascii=False, indent=2)

  print(f"Generated cases at: {out_dir}")
  print(f"Cases: {len(cases)}")
  print(f"Targets met: {len(all_targets) - len(unmet_targets)}/{len(all_targets)}")
  print(f"Manifest: {manifest_path}")
  return 0 if not unmet_targets else 2


if __name__ == "__main__":
  raise SystemExit(main())
