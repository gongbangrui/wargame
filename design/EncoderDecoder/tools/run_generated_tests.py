#!/usr/bin/env python3
import argparse
import glob
import json
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional


@dataclass
class CmdResult:
  code: int
  out: str
  err: str


def run_cmd(args: List[str]) -> CmdResult:
  p = subprocess.run(args, capture_output=True, text=True)
  return CmdResult(p.returncode, p.stdout, p.stderr)


def read_manifest(cases_dir: Path) -> Optional[Dict]:
  p = cases_dir / "cases_manifest.json"
  if not p.exists():
    return None
  with p.open("r", encoding="utf-8") as f:
    return json.load(f)


def parse_error_type_from_header(xml_path: Path) -> str:
  with xml_path.open("r", encoding="utf-8") as f:
    head = "".join([f.readline() for _ in range(3)])
  m = re.search(r"ERROR_TYPE:\s*([A-Z_]+|NONE)", head)
  if not m:
    return "NONE"
  return m.group(1)


def collect_cases(cases_dir: Path) -> List[Dict[str, str]]:
  manifest = read_manifest(cases_dir)
  if manifest and "cases" in manifest:
    return manifest["cases"]

  out: List[Dict[str, str]] = []
  for p in sorted(glob.glob(str(cases_dir / "**" / "*.xml"), recursive=True)):
    xp = Path(p)
    et = parse_error_type_from_header(xp)
    category = "positive" if et == "NONE" else ("negative_rules" if et == "RULE_VIOLATION" else "negative_builtin")
    out.append({
      "path": str(xp),
      "category": category,
      "error_type": et,
    })
  return out


def summarize_output(r: CmdResult) -> str:
  s = ((r.out or "") + (r.err or "")).strip().splitlines()
  return " | ".join(s[:4])


def expected_class(case: Dict[str, str]) -> str:
  if case.get("category") == "positive":
    return "positive"
  et = case.get("error_type", "NONE")
  if et == "NONE":
    return "positive"
  if et == "RULE_VIOLATION":
    return "expected_rule_fail"
  return "expected_builtin_fail"


def normalize_length_for_compare(src_xml: Path, decoded_xml: Path, out_xml: Path) -> None:
  src_tree = ET.parse(src_xml)
  dec_tree = ET.parse(decoded_xml)
  src_root = src_tree.getroot()
  dec_root = dec_tree.getroot()

  src_header = src_root.find("Header")
  dec_header = dec_root.find("Header")
  if src_header is None or dec_header is None:
    src_tree.write(out_xml, encoding="utf-8", xml_declaration=True)
    return

  src_len = None
  dec_len = None
  for f in list(src_header):
    if f.tag == "Field" and f.attrib.get("name") == "length":
      src_len = f
      break
  for f in list(dec_header):
    if f.tag == "Field" and f.attrib.get("name") == "length":
      dec_len = f
      break
  if src_len is not None and dec_len is not None:
    src_len.text = dec_len.text
  src_tree.write(out_xml, encoding="utf-8", xml_declaration=True)


def main() -> int:
  ap = argparse.ArgumentParser(description="Run generated VMF cases with validate-first pipeline.")
  ap.add_argument("--dict", dest="dict_path", default="dic.xml")
  ap.add_argument("--content-dict", dest="content_dict_path", default=None)
  ap.add_argument("--cases", dest="cases_dir", default="generated")
  ap.add_argument("--build-dir", default="build")
  args = ap.parse_args()

  dict_path = Path(args.dict_path).resolve()
  content_dict_path = Path(args.content_dict_path).resolve() if args.content_dict_path else None
  if content_dict_path is None:
    raise ValueError("--content-dict is required because vmf_encode/vmf_decode now need dic_content.xml")
  cases_dir = Path(args.cases_dir).resolve()
  build_dir = Path(args.build_dir).resolve()

  bins = {
    "validate": build_dir / "vmf_validate",
    "encode": build_dir / "vmf_encode",
    "decode": build_dir / "vmf_decode",
    "compare": build_dir / "xml_compare",
  }
  for n, p in bins.items():
    if not p.exists():
      raise FileNotFoundError(f"missing binary {n}: {p}")

  cases = collect_cases(cases_dir)
  report_cases = []
  counters = {
    "total": 0,
    "pipeline_ok": 0,
    "unexpected_fail": 0,
  }

  for case in cases:
    counters["total"] += 1
    case_path = Path(case["path"])
    exp = expected_class(case)

    validate_args = [str(bins["validate"]), str(dict_path)]
    if content_dict_path is not None:
      validate_args.append(str(content_dict_path))
    validate_args.append(str(case_path))
    vr = run_cmd(validate_args)
    vout = summarize_output(vr)

    rec = {
      "path": str(case_path),
      "expected": exp,
      "error_type": case.get("error_type", "NONE"),
      "primary_target": case.get("primary_target"),
      "covered_targets": case.get("covered_targets", []),
      "forced_paths": case.get("forced_paths", []),
      "forced_values": case.get("forced_values", {}),
      "repeat_settings": case.get("repeat_settings", {}),
      "selectable_settings": case.get("selectable_settings", {}),
      "repair_actions": case.get("repair_actions", []),
      "validate_attempts_planned": case.get("validate_attempts"),
      "validate_exit": vr.code,
      "validate_output": vout,
      "pipeline_executed": False,
      "pipeline": {},
      "classification": "",
    }

    if exp == "positive":
      if vr.code != 0:
        rec["classification"] = "unexpected_fail"
        counters["unexpected_fail"] += 1
      else:
        rec["pipeline_executed"] = True
        with tempfile.TemporaryDirectory(prefix="vmf_gen_run_") as td:
          td_path = Path(td)
          bin_path = td_path / "out.bin"
          dec_path = td_path / "decoded.xml"
          er = run_cmd([str(bins["encode"]), str(dict_path), str(content_dict_path), str(case_path), str(bin_path)])
          dr = CmdResult(-1, "", "")
          cr = CmdResult(-1, "", "")
          if er.code == 0:
            dr = run_cmd([str(bins["decode"]), str(dict_path), str(content_dict_path), str(bin_path), str(dec_path)])
          if er.code == 0 and dr.code == 0:
            norm_src = td_path / "normalized_src.xml"
            normalize_length_for_compare(case_path, dec_path, norm_src)
            cr = run_cmd([str(bins["compare"]), str(norm_src), str(dec_path)])

          rec["pipeline"] = {
            "encode_exit": er.code,
            "encode_output": summarize_output(er),
            "decode_exit": dr.code,
            "decode_output": summarize_output(dr),
            "compare_exit": cr.code,
            "compare_output": summarize_output(cr),
          }
          if er.code == 0 and dr.code == 0 and cr.code == 0:
            rec["classification"] = "pipeline_ok"
            counters["pipeline_ok"] += 1
          else:
            rec["classification"] = "unexpected_fail"
            counters["unexpected_fail"] += 1
    else:
      rec["classification"] = "unexpected_fail"
      counters["unexpected_fail"] += 1

    report_cases.append(rec)

  report = {
    "dict": str(dict_path),
    "content_dict": str(content_dict_path) if content_dict_path else None,
    "cases_dir": str(cases_dir),
    "summary": counters,
    "cases": report_cases,
  }
  report_path = cases_dir / "report.json"
  report_path.parent.mkdir(parents=True, exist_ok=True)
  with report_path.open("w", encoding="utf-8") as f:
    json.dump(report, f, ensure_ascii=False, indent=2)

  print(f"Report written: {report_path}")
  print(json.dumps(counters, ensure_ascii=False))

  return 0 if counters["unexpected_fail"] == 0 else 2


if __name__ == "__main__":
  raise SystemExit(main())
