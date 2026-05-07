#!/usr/bin/env python3
import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

try:
    import pandas as pd
except ImportError as exc:
    raise SystemExit(
        "Pandas nao encontrado. Instale com: pip install pandas matplotlib"
    ) from exc


def read_table(path, names, comment="%"):
    if not path.exists():
        return None
    df = pd.read_csv(
        path,
        delim_whitespace=True,
        comment=comment,
        names=names,
        header=None,
        engine="python",
    )
    if df.empty:
        return None
    return df


def select_ue_column(df, preferred=("IMSI", "imsi", "rnti", "RNTI")):
    for col in preferred:
        if col in df.columns:
            values = df[col].dropna().unique()
            if len(values) == 0:
                continue
            if len(values) == 1 and values[0] == 0:
                continue
            return col
    for col in df.columns:
        if col.lower() in ("imsi", "rnti"):
            return col
    return None


def linear_to_db(series):
    series = series.clip(lower=1e-20)
    return 10.0 * np.log10(series)


def plot_time_series(df, time_col, value_col, ue_col, out_path, title, ylabel, max_ues=5):
    if df is None or ue_col is None:
        return False

    fig, ax = plt.subplots(figsize=(10, 5))
    ues = sorted(df[ue_col].unique())[:max_ues]
    for ue in ues:
        subset = df[df[ue_col] == ue]
        ax.plot(subset[time_col], subset[value_col], label=f"{ue_col}={ue}", linewidth=1.2)

    ax.set_title(title)
    ax.set_xlabel("Tempo (s)")
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    return True


def plot_handover_timeline(start_df, end_df, out_path, max_ues=20):
    if start_df is None and end_df is None:
        return False

    fig, ax = plt.subplots(figsize=(10, 5))

    ue_col = None
    if start_df is not None:
        ue_col = select_ue_column(start_df, preferred=("imsi", "rnti"))
    if ue_col is None and end_df is not None:
        ue_col = select_ue_column(end_df, preferred=("imsi", "rnti"))
    if ue_col is None:
        return False

    if start_df is not None:
        ues = sorted(start_df[ue_col].unique())[:max_ues]
    else:
        ues = sorted(end_df[ue_col].unique())[:max_ues]

    y_map = {ue: idx for idx, ue in enumerate(ues)}

    if start_df is not None:
        for _, row in start_df.iterrows():
            ue = row[ue_col]
            if ue not in y_map:
                continue
            ax.scatter(row["time"], y_map[ue], color="#1f77b4", s=18, marker="v")

    if end_df is not None:
        for _, row in end_df.iterrows():
            ue = row[ue_col]
            if ue not in y_map:
                continue
            ax.scatter(row["time"], y_map[ue], color="#ff7f0e", s=18, marker="^")

    ax.set_title("Linha do tempo de handover (UE)")
    ax.set_xlabel("Tempo (s)")
    ax.set_ylabel("UE (IMSI/RNTI)")
    ax.set_yticks(list(y_map.values()))
    ax.set_yticklabels(list(y_map.keys()))
    ax.grid(True, axis="x", alpha=0.3)
    ax.legend(["Start", "End"], loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    return True


def plot_pdcp_kpms(pdcp_df, out_dir, prefix, max_ues=5):
    if pdcp_df is None:
        return False

    pdcp_df = pdcp_df.copy()
    pdcp_df["duration"] = (pdcp_df["end"] - pdcp_df["start"]).clip(lower=1e-9)
    pdcp_df["midTime"] = (pdcp_df["start"] + pdcp_df["end"]) / 2.0
    pdcp_df["throughputKbps"] = (pdcp_df["TxBytes"] * 8.0) / pdcp_df["duration"] / 1000.0

    ue_col = select_ue_column(pdcp_df, preferred=("IMSI", "RNTI"))
    plot_time_series(
        pdcp_df,
        "midTime",
        "throughputKbps",
        ue_col,
        out_dir / f"{prefix}_pdcp_throughput_kbps.png",
        f"{prefix.upper()} PDCP Throughput (UE)",
        "Throughput (kbps)",
        max_ues=max_ues,
    )
    plot_time_series(
        pdcp_df,
        "midTime",
        "delay",
        ue_col,
        out_dir / f"{prefix}_pdcp_delay_s.png",
        f"{prefix.upper()} PDCP Delay (UE)",
        "Delay (s)",
        max_ues=max_ues,
    )
    return True


def main():
    parser = argparse.ArgumentParser(description="Plota graficos a partir da pasta outputs")
    parser.add_argument("--outputs-dir", default="outputs/scenario-three")
    parser.add_argument("--out-dir", default="outputs/scenario-three/plots")
    parser.add_argument("--max-ues", type=int, default=5)
    args = parser.parse_args()

    outputs_dir = Path(args.outputs_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    dl_path = outputs_dir / "LteDlRsrpSinrStats.txt"
    ul_path = outputs_dir / "LteUlSinrStats.txt"
    mmw_path = outputs_dir / "MmWaveSinrTime.txt"
    ho_start_path = outputs_dir / "UeHandoverStartStats.txt"
    ho_end_path = outputs_dir / "UeHandoverEndStats.txt"
    dl_pdcp_path = outputs_dir / "DlPdcpStats.txt"
    ul_pdcp_path = outputs_dir / "UlPdcpStats.txt"

    dl_df = read_table(
        dl_path,
        names=["time", "cellId", "IMSI", "RNTI", "rsrp", "sinr", "componentCarrierId"],
    )
    ul_df = read_table(
        ul_path,
        names=["time", "cellId", "IMSI", "RNTI", "sinrLinear", "componentCarrierId"],
    )
    mmw_df = read_table(
        mmw_path,
        names=["time", "cellId", "rnti", "sinrDb"],
        comment=None,
    )

    if dl_df is not None:
        dl_df["rsrpDbm"] = linear_to_db(dl_df["rsrp"]) + 30.0
        dl_df["sinrDb"] = linear_to_db(dl_df["sinr"])

    if ul_df is not None:
        ul_df["sinrDb"] = linear_to_db(ul_df["sinrLinear"])

    ho_start_df = read_table(
        ho_start_path,
        names=["time", "imsi", "rnti", "sourceCellId", "targetCellId"],
        comment=None,
    )
    ho_end_df = read_table(
        ho_end_path,
        names=["time", "imsi", "rnti", "targetCellId"],
        comment=None,
    )

    pdcp_cols = [
        "start",
        "end",
        "CellId",
        "IMSI",
        "RNTI",
        "LCID",
        "nTxPDUs",
        "TxBytes",
        "nRxPDUs",
        "RxBytes",
        "delay",
        "delayStdDev",
        "delayMin",
        "delayMax",
        "pduSizeMean",
        "pduSizeStdDev",
        "pduSizeMin",
        "pduSizeMax",
    ]
    dl_pdcp_df = read_table(dl_pdcp_path, names=pdcp_cols)
    ul_pdcp_df = read_table(ul_pdcp_path, names=pdcp_cols)

    ue_col_dl = select_ue_column(dl_df, preferred=("IMSI", "RNTI")) if dl_df is not None else None
    ue_col_ul = select_ue_column(ul_df, preferred=("IMSI", "RNTI")) if ul_df is not None else None

    plot_time_series(
        dl_df,
        "time",
        "rsrpDbm",
        ue_col_dl,
        out_dir / "lte_dl_rsrp_dbm.png",
        "LTE DL RSRP (UE)",
        "RSRP (dBm)",
        max_ues=args.max_ues,
    )
    plot_time_series(
        dl_df,
        "time",
        "sinrDb",
        ue_col_dl,
        out_dir / "lte_dl_sinr_db.png",
        "LTE DL SINR (UE)",
        "SINR (dB)",
        max_ues=args.max_ues,
    )
    plot_time_series(
        ul_df,
        "time",
        "sinrDb",
        ue_col_ul,
        out_dir / "lte_ul_sinr_db.png",
        "LTE UL SINR (UE)",
        "SINR (dB)",
        max_ues=args.max_ues,
    )

    if mmw_df is not None:
        ue_col_mmw = select_ue_column(mmw_df, preferred=("rnti",))
        plot_time_series(
            mmw_df,
            "time",
            "sinrDb",
            ue_col_mmw,
            out_dir / "mmwave_sinr_db.png",
            "mmWave SINR (UE)",
            "SINR (dB)",
            max_ues=args.max_ues,
        )

    plot_handover_timeline(
        ho_start_df,
        ho_end_df,
        out_dir / "handover_timeline.png",
        max_ues=min(args.max_ues * 2, 20),
    )

    plot_pdcp_kpms(dl_pdcp_df, out_dir, "dl", max_ues=args.max_ues)
    plot_pdcp_kpms(ul_pdcp_df, out_dir, "ul", max_ues=args.max_ues)

    print(f"Graficos gerados em: {out_dir}")


if __name__ == "__main__":
    main()
