#!/usr/bin/env python3
import argparse
import re
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch, Rectangle as MplRectangle
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


def read_csv_if_exists(path):
    if not path.exists():
        return None
    df = pd.read_csv(path)
    if df.empty:
        return None
    return df

def read_buildings(path):
    data = read_csv_if_exists(path)
    required = {"center_x_m", "center_y_m", "width_x_m", "depth_y_m"}
    if data is None or not required.issubset(data.columns):
        return None
    return [
        (
            row["center_x_m"] - row["width_x_m"] / 2.0,
            row["center_y_m"] - row["depth_y_m"] / 2.0,
            row["width_x_m"],
            row["depth_y_m"],
            "",
        )
        for _, row in data.iterrows()
    ]


def read_enb_positions(path):
    if not path.exists():
        return pd.DataFrame(columns=["cell_id", "x_m", "y_m", "kind"])

    rows = []
    pattern = re.compile(r'set label "(?P<cell>\d+)" at (?P<x>-?\d+(?:\.\d+)?),(?P<y>-?\d+(?:\.\d+)?).*rgb "(?P<color>\w+)"')
    for line in path.read_text().splitlines():
        match = pattern.search(line)
        if not match:
            continue
        color = match.group("color").lower()
        rows.append(
            {
                "cell_id": int(match.group("cell")),
                "x_m": float(match.group("x")),
                "y_m": float(match.group("y")),
                "kind": "LTE" if color == "blue" else "mmWave",
            }
        )
    return pd.DataFrame(rows)


def scenario_zero_buildings():
    center_x = 2000.0
    center_y = 2000.0
    isd = 1200.0
    triangle_radius = isd / np.sqrt(3.0)
    gnbs = [
        (center_x, center_y + triangle_radius),
        (center_x - isd / 2.0, center_y - triangle_radius / 2.0),
        (center_x + isd / 2.0, center_y - triangle_radius / 2.0),
    ]

    buildings = []

    def add_box(x_center, y_center, width, height, label):
        buildings.append((x_center - width / 2.0, y_center - height / 2.0, width, height, label))

    for idx, (x, y) in enumerate(gnbs, start=1):
        add_box(x + 115.0, y + 95.0, 120.0, 105.0, f"G{idx}-A")
        add_box(x - 125.0, y - 105.0, 135.0, 115.0, f"G{idx}-B")

    for i, (ax, ay) in enumerate(gnbs):
        bx, by = gnbs[(i + 1) % len(gnbs)]
        dx = bx - ax
        dy = by - ay
        length = np.hypot(dx, dy)
        nx = -dy / length
        ny = dx / length
        for j in range(1, 4):
            t = j / 4.0
            side = -1.0 if j % 2 == 0 else 1.0
            x = ax + t * dx + side * nx * 115.0
            y = ay + t * dy + side * ny * 115.0
            if i == 0 and j == 1:
                x -= 40.0
            elif i == 2 and j == 3:
                x += 40.0
            if (i == 0 and j == 3) or (i == 2 and j == 1):
                y += 40.0
            width = 210.0 if j == 2 else 155.0
            height = 85.0 if j == 2 else 120.0
            add_box(x, y, width, height, f"C{i + 1}-{j}")

    add_box(center_x, center_y + isd * 0.14, 230.0, 90.0, "M1")
    add_box(center_x - isd * 0.18, center_y + isd * 0.03, 170.0, 130.0, "M2")
    add_box(center_x + isd * 0.18, center_y + isd * 0.03, 170.0, 130.0, "M3")
    add_box(center_x, center_y - isd * 0.20, 260.0, 105.0, "M4")
    return buildings



def plot_per_ue_metric_by_cell(data, value_col, ylabel, suffix, out_dir):
    preferred_source = "MMWAVE-PHY" if "MMWAVE-PHY" in set(data["source"]) else sorted(data["source"].unique())[0]
    source_data = data[data["source"] == preferred_source].copy()
    if source_data.empty:
        return 0

    best_cell = (
        source_data.sort_values(value_col, ascending=False)
        .drop_duplicates(["time_s", "ue"])
        .sort_values(["ue", "time_s"])
    )
    if best_cell.empty:
        return 0

    ues = sorted(best_cell["ue"].unique())
    cells = sorted(best_cell["cell_id"].unique())
    cell_palette = ["#0072B2", "#D55E00", "#009E73", "#CC79A7", "#E69F00", "#56B4E9"]
    cell_colors = {cell: cell_palette[idx % len(cell_palette)] for idx, cell in enumerate(cells)}

    cols = 2 if len(ues) <= 6 else 3
    rows = int(np.ceil(len(ues) / cols))
    fig, axes = plt.subplots(rows, cols, figsize=(5.0 * cols, 2.6 * rows), sharex=True, sharey=True)
    axes = np.atleast_1d(axes).ravel()

    for ax, ue in zip(axes, ues):
        subset = best_cell[best_cell["ue"] == ue].sort_values("time_s")
        if len(subset) == 1:
            row = subset.iloc[0]
            ax.scatter(row["time_s"], row[value_col], color=cell_colors[row["cell_id"]], s=18)
        else:
            rows_iter = list(subset.itertuples(index=False))
            for current, nxt in zip(rows_iter[:-1], rows_iter[1:]):
                ax.plot(
                    [current.time_s, nxt.time_s],
                    [getattr(current, value_col), getattr(nxt, value_col)],
                    color=cell_colors[current.cell_id],
                    linewidth=1.35,
                )
        ax.set_title(f"UE {ue}", fontsize=10)
        ax.grid(True, alpha=0.25)

    for ax in axes[len(ues):]:
        ax.set_visible(False)

    for ax in axes[::cols]:
        ax.set_ylabel(ylabel)
    for ax in axes[-cols:]:
        if ax.get_visible():
            ax.set_xlabel("Tempo (s)")

    handles = [
        Line2D([0], [0], color=cell_colors[cell], lw=2, label=f"celula {cell}")
        for cell in cells
    ]
    fig.suptitle(f"{preferred_source} - {ylabel} por UE, cor pela celula escolhida", y=0.995)
    fig.legend(handles=handles, loc="upper center", ncol=min(len(handles), 4), bbox_to_anchor=(0.5, 0.965))
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(out_dir / f"dataset_{suffix}_per_ue_by_cell.png", dpi=150)
    plt.close(fig)
    return 1


def plot_all_ues_per_gnb(data, value_col, ylabel, suffix, out_dir):
    preferred_source = "MMWAVE-PHY" if "MMWAVE-PHY" in set(data["source"]) else sorted(data["source"].unique())[0]
    source_data = data[data["source"] == preferred_source].copy()
    if source_data.empty:
        return 0

    generated = 0
    ue_palette = [
        "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd",
        "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf",
        "#393b79", "#637939", "#8c6d31", "#843c39", "#7b4173",
    ]
    ues = sorted(source_data["ue"].unique())
    ue_colors = {ue: ue_palette[idx % len(ue_palette)] for idx, ue in enumerate(ues)}

    for cell_id, cell_data in source_data.groupby("cell_id", sort=True):
        fig, ax = plt.subplots(figsize=(12, 6))
        for ue, subset in cell_data.groupby("ue", sort=True):
            subset = subset.sort_values("time_s")
            ax.plot(
                subset["time_s"],
                subset[value_col],
                linewidth=1.15,
                color=ue_colors[ue],
                label=f"UE {ue}",
            )

        ax.set_title(f"{preferred_source} - {ylabel} de todos os UEs na gNB/celula {cell_id}")
        ax.set_xlabel("Tempo (s)")
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.3)
        ax.legend(loc="best", fontsize=8, ncol=2)
        fig.tight_layout()
        fig.savefig(out_dir / f"dataset_{suffix}_cell_{int(cell_id)}_all_ues.png", dpi=150)
        plt.close(fig)
        generated += 1

    return generated


def plot_per_ue_all_gnbs(data, value_col, ylabel, suffix, out_dir):
    preferred_source = "MMWAVE-PHY" if "MMWAVE-PHY" in set(data["source"]) else sorted(data["source"].unique())[0]
    source_data = data[data["source"] == preferred_source].copy()
    if source_data.empty:
        return 0

    cell_palette = ["#0072B2", "#D55E00", "#009E73", "#CC79A7", "#E69F00", "#56B4E9"]
    cells = sorted(source_data["cell_id"].unique())
    cell_colors = {cell: cell_palette[idx % len(cell_palette)] for idx, cell in enumerate(cells)}

    generated = 0
    for ue, ue_data in source_data.groupby("ue", sort=True):
        fig, ax = plt.subplots(figsize=(10, 5.5))
        plotted = 0
        for cell_id, subset in ue_data.groupby("cell_id", sort=True):
            subset = subset.sort_values("time_s")
            if subset.empty:
                continue
            ax.plot(
                subset["time_s"],
                subset[value_col],
                linewidth=1.5,
                color=cell_colors[cell_id],
                label=f"gNB/celula {int(cell_id)}",
            )
            plotted += 1

        if plotted == 0:
            plt.close(fig)
            continue

        ax.set_title(f"{preferred_source} - UE {int(ue) if float(ue).is_integer() else ue}: {ylabel} por gNB")
        ax.set_xlabel("Tempo (s)")
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.3)
        ax.legend(loc="best", fontsize=9)
        fig.tight_layout()
        fig.savefig(out_dir / f"dataset_{suffix}_ue_{int(ue) if float(ue).is_integer() else ue}_all_gnbs.png", dpi=150)
        plt.close(fig)
        generated += 1

    return generated


def plot_signal_dataset(signal_df, out_dir, max_ues=5):
    if signal_df is None:
        return 0

    generated = 0
    signal_df = signal_df.copy()
    signal_df["ue"] = signal_df["imsi"].where(signal_df["imsi"].fillna(0) != 0, signal_df["rnti"])
    signal_df = signal_df[signal_df["ue"].fillna(0) != 0]
    signal_df["series"] = signal_df["source"].astype(str) + " celula=" + signal_df["cell_id"].astype(str)

    for value_col, ylabel, suffix in (
        ("rsrp_dbm", "RSRP (dBm)", "rsrp_dbm"),
        ("sinr_db", "SINR (dB)", "sinr_db"),
        ("rsrq_db", "RSRQ (dB)", "rsrq_db"),
    ):
        data = signal_df.dropna(subset=[value_col])
        if data.empty:
            continue

        summary = (
            data.groupby(["time_s", "source", "cell_id"], as_index=False)[value_col]
            .mean()
            .sort_values(["source", "cell_id", "time_s"])
        )
        fig, ax = plt.subplots(figsize=(10, 5))
        ue_count = data["ue"].nunique()
        for (source, cell_id), subset in summary.groupby(["source", "cell_id"], sort=True):
            ax.plot(
                subset["time_s"],
                subset[value_col],
                linewidth=1.6,
                label=f"media {source} | celula {cell_id} | {ue_count} UEs",
            )

        ax.set_title(f"{ylabel} medio por celula/fonte")
        ax.set_xlabel("Tempo (s)")
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.3)
        ax.legend(loc="best", fontsize=8)
        fig.tight_layout()
        fig.savefig(out_dir / f"dataset_{suffix}.png", dpi=150)
        plt.close(fig)
        generated += 1

        best_cell = (
            data.sort_values(value_col, ascending=False)
            .drop_duplicates(["time_s", "source", "ue"])
            .sort_values(["source", "ue", "time_s"])
        )

        preferred_source = "MMWAVE-PHY" if "MMWAVE-PHY" in set(best_cell["source"]) else sorted(best_cell["source"].unique())[0]
        single_source = best_cell[best_cell["source"] == preferred_source]

        fig, ax = plt.subplots(figsize=(11, 5.5))
        ues = sorted(single_source["ue"].unique())[:max_ues]
        selected = single_source[single_source["ue"].isin(ues)]
        for ue, subset in selected.groupby("ue", sort=True):
            ax.plot(
                subset["time_s"],
                subset[value_col],
                linewidth=1.25,
                label=f"UE {ue}",
            )

        ax.set_title(f"{preferred_source} - {ylabel} por UE usando a melhor celula")
        ax.set_xlabel("Tempo (s)")
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.3)
        ax.legend(loc="best", fontsize=8, ncol=2)
        fig.tight_layout()
        fig.savefig(out_dir / f"dataset_{suffix}_best_cell_selected_ues.png", dpi=150)
        plt.close(fig)
        generated += 1

        generated += plot_per_ue_metric_by_cell(data, value_col, ylabel, suffix, out_dir)
        generated += plot_all_ues_per_gnb(data, value_col, ylabel, suffix, out_dir)
        generated += plot_per_ue_all_gnbs(data, value_col, ylabel, suffix, out_dir)

        for source, source_data in best_cell.groupby("source", sort=True):
            pivot = source_data.pivot_table(
                index="ue",
                columns="time_s",
                values=value_col,
                aggfunc="mean",
            ).sort_index()
            if pivot.empty:
                continue

            fig, ax = plt.subplots(figsize=(11, 5))
            image = ax.imshow(pivot, aspect="auto", interpolation="nearest", origin="lower")
            time_values = list(pivot.columns)
            if time_values:
                tick_count = min(8, len(time_values))
                tick_positions = np.linspace(0, len(time_values) - 1, tick_count, dtype=int)
                ax.set_xticks(tick_positions)
                ax.set_xticklabels([f"{time_values[i]:.1f}" for i in tick_positions])
            ax.set_yticks(np.arange(len(pivot.index)))
            ax.set_yticklabels([
                str(int(ue)) if float(ue).is_integer() else str(ue)
                for ue in pivot.index
            ])
            ax.set_title(f"{source} - mapa de calor {ylabel} por UE")
            ax.set_xlabel("Tempo (s)")
            ax.set_ylabel("UE")
            fig.colorbar(image, ax=ax, label=ylabel)
            fig.tight_layout()
            safe_source = str(source).lower().replace("-", "_")
            fig.savefig(out_dir / f"dataset_{suffix}_{safe_source}_heatmap.png", dpi=150)
            plt.close(fig)
            generated += 1

    return generated


def plot_ue_positions(position_df, out_dir, max_ues=10, enb_df=None, buildings=None):
    if position_df is None:
        return 0

    generated = 0
    data = position_df.copy()
    ue_col = "imsi" if "imsi" in data.columns else "node_id"
    ues = sorted(data[ue_col].dropna().unique())[:max_ues]
    data = data[data[ue_col].isin(ues)]

    palette = [
        "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd",
        "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf",
        "#393b79", "#637939", "#8c6d31", "#843c39", "#7b4173",
    ]
    ue_colors = {ue: palette[idx % len(palette)] for idx, ue in enumerate(ues)}

    fig, ax = plt.subplots(figsize=(8, 8))

    building_handles = []
    if buildings:
        for x, y, width, height, label in buildings:
            rect = MplRectangle(
                (x, y),
                width,
                height,
                facecolor="#b0b0b0",
                edgecolor="#4d4d4d",
                linewidth=0.9,
                alpha=0.35,
                zorder=0,
            )
            ax.add_patch(rect)
        building_handles.append(Patch(facecolor="#b0b0b0", edgecolor="#4d4d4d", alpha=0.35, label="Predios"))

    enb_handles = []
    if enb_df is not None and not enb_df.empty:
        for kind, subset in enb_df.groupby("kind", sort=True):
            marker = "^" if kind == "mmWave" else "s"
            color = "#c51b29" if kind == "mmWave" else "#08519c"
            ax.scatter(
                subset["x_m"],
                subset["y_m"],
                s=95,
                marker=marker,
                color=color,
                edgecolors="black",
                linewidths=0.7,
                zorder=5,
                label=f"Antena {kind}",
            )
            enb_handles.append(Line2D([0], [0], marker=marker, color="w", markerfacecolor=color, markeredgecolor="black", markersize=9, label=f"Antena {kind}"))
        for _, row in enb_df.iterrows():
            ax.annotate(
                f"cel {int(row['cell_id'])}",
                (row["x_m"], row["y_m"]),
                xytext=(6, 6),
                textcoords="offset points",
                fontsize=8,
                color="#111111",
                zorder=6,
            )

    for ue, subset in data.groupby(ue_col, sort=True):
        color = ue_colors[ue]
        ax.plot(subset["x_m"], subset["y_m"], linewidth=1.35, color=color, label=f"UE {ue}")
        ax.scatter(
            subset["x_m"].iloc[0],
            subset["y_m"].iloc[0],
            s=34,
            marker="o",
            color=color,
            edgecolors="black",
            linewidths=0.5,
            zorder=3,
        )
        ax.scatter(
            subset["x_m"].iloc[-1],
            subset["y_m"].iloc[-1],
            s=42,
            marker="X",
            color=color,
            edgecolors="black",
            linewidths=0.5,
            zorder=3,
        )
    ax.set_title("Trajetoria dos UEs (circulo=inicio, X=fim)")
    ax.set_xlabel("x (m)")
    ax.set_ylabel("y (m)")
    ax.grid(True, alpha=0.3)
    ax.axis("equal")
    first_legend = ax.legend(loc="best", fontsize=8, title="UEs")
    ax.add_artist(first_legend)
    context_handles = building_handles + enb_handles
    if context_handles:
        ax.legend(handles=context_handles, loc="upper right", fontsize=8, title="Cenario")
    fig.tight_layout()
    fig.savefig(out_dir / "ue_trajectories_xy.png", dpi=150)
    plt.close(fig)
    generated += 1

    for coord, ylabel in (("x_m", "x (m)"), ("y_m", "y (m)")):
        fig, ax = plt.subplots(figsize=(10, 5))
        for ue, subset in data.groupby(ue_col, sort=True):
            ax.plot(subset["time_s"], subset[coord], linewidth=1.2, color=ue_colors[ue], label=f"UE {ue}")
        ax.set_title(f"Posicao dos UEs - {ylabel}")
        ax.set_xlabel("Tempo (s)")
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.3)
        ax.legend(loc="best", fontsize=8)
        fig.tight_layout()
        fig.savefig(out_dir / f"ue_position_{coord}.png", dpi=150)
        plt.close(fig)
        generated += 1

    if {"vx_mps", "vy_mps"}.issubset(data.columns):
        data["speed_mps"] = np.sqrt(data["vx_mps"] ** 2 + data["vy_mps"] ** 2)
        fig, ax = plt.subplots(figsize=(10, 5))
        for ue, subset in data.groupby(ue_col, sort=True):
            ax.plot(subset["time_s"], subset["speed_mps"], linewidth=1.2, color=ue_colors[ue], label=f"UE {ue}")
        ax.set_title("Velocidade dos UEs")
        ax.set_xlabel("Tempo (s)")
        ax.set_ylabel("Velocidade (m/s)")
        ax.grid(True, alpha=0.3)
        ax.legend(loc="best", fontsize=8)
        fig.tight_layout()
        fig.savefig(out_dir / "ue_speed_mps.png", dpi=150)
        plt.close(fig)
        generated += 1

    return generated


def main():
    parser = argparse.ArgumentParser(description="Plota graficos a partir da pasta outputs")
    parser.add_argument("--outputs-dir", default="outputs/scenario-zero")
    parser.add_argument("--out-dir", default=None)
    parser.add_argument("--max-ues", type=int, default=5)
    args = parser.parse_args()

    outputs_dir = Path(args.outputs_dir)
    out_dir = Path(args.out_dir) if args.out_dir else outputs_dir / "plots"
    out_dir.mkdir(parents=True, exist_ok=True)

    signal_dataset_path = outputs_dir / "dataset_lstm.csv"
    position_path = outputs_dir / "ue_positions.csv"
    app_metrics_path = outputs_dir / "app_metrics.csv"
    enb_path = outputs_dir / "enbs.txt"
    building_path = outputs_dir / "buildings.csv"
    dl_path = outputs_dir / "LteDlRsrpSinrStats.txt"
    ul_path = outputs_dir / "LteUlSinrStats.txt"
    mmw_path = outputs_dir / "MmWaveSinrTime.txt"
    ho_start_path = outputs_dir / "UeHandoverStartStats.txt"
    ho_end_path = outputs_dir / "UeHandoverEndStats.txt"
    dl_pdcp_path = outputs_dir / "DlPdcpStats.txt"
    ul_pdcp_path = outputs_dir / "UlPdcpStats.txt"

    signal_dataset_df = read_csv_if_exists(signal_dataset_path)
    position_df = read_csv_if_exists(position_path)
    app_metrics_df = read_csv_if_exists(app_metrics_path)
    enb_df = read_enb_positions(enb_path)
    buildings = read_buildings(building_path)

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

    generated = 0
    generated += plot_signal_dataset(signal_dataset_df, out_dir, max_ues=args.max_ues)
    generated += plot_ue_positions(
        position_df,
        out_dir,
        max_ues=args.max_ues,
        enb_df=enb_df,
        buildings=buildings if buildings is not None else scenario_zero_buildings(),
    )

    if app_metrics_df is not None:
        plot_time_series(
            app_metrics_df,
            "time_s",
            "throughput_kbps",
            "imsi",
            out_dir / "app_throughput_kbps.png",
            "Throughput downlink por UE",
            "Throughput (kbps)",
            max_ues=args.max_ues,
        )
        plot_time_series(
            app_metrics_df,
            "time_s",
            "jitter_ms",
            "imsi",
            out_dir / "app_jitter_ms.png",
            "Jitter downlink por UE",
            "Jitter entre pacotes (ms)",
            max_ues=args.max_ues,
        )

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
        max_ues=args.max_ues,
    )

    plot_pdcp_kpms(dl_pdcp_df, out_dir, "dl", max_ues=args.max_ues)
    plot_pdcp_kpms(ul_pdcp_df, out_dir, "ul", max_ues=args.max_ues)

    if generated == 0 and signal_dataset_df is None and position_df is None:
        print("Aviso: dataset_lstm.csv/ue_positions.csv nao encontrados; foram tentados apenas os traces legados.")
    print(f"Graficos gerados em: {out_dir}")


if __name__ == "__main__":
    main()
