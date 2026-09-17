#!/usr/bin/env python3
"""
NovexOS Build Dependencies Installer
=====================================
Application GUI (Tkinter, stdlib uniquement) pour installer les dépendances
nécessaires au build de NovexOS, selon l'OS détecté.

- Linux   : passe par apt-get (nécessite les droits root -> mot de passe sudo demandé)
- Windows : installation SANS droits admin par défaut (mode Portable), ou installation
            propre dans les dossiers système (mode Admin, PATH permanent). Une option
            "WSL - outils ISO" facultative et légère permet en plus de générer une
            vraie ISO bootable (grub-mkrescue n'existe pas nativement sous Windows) :
            elle installe WSL/Debian si absent (léger, apt identique à Ubuntu) puis
            SEULEMENT xorriso/grub/mtools dedans — make, le cross-compiler et QEMU
            restent natifs Windows, inchangés.
- macOS   : passe par Homebrew (non officiellement supporté par NovexOS, best-effort)

Une fois l'installation terminée, l'appli génère un script d'environnement
(env.bat sous Windows, env.sh sous Linux/macOS) à sourcer/exécuter avant de
lancer `make build`, pour que les outils installés dans le dossier choisi
soient bien dans le PATH de la session.

IMPORTANT : les URLs de téléchargement (surtout QEMU sous Windows et le tap
Homebrew macOS) peuvent devenir obsolètes avec le temps. Elles sont toutes
centralisées dans DEPENDENCIES ci-dessous pour être faciles à mettre à jour.
"""

import json
import os
import platform
import re
from collections import deque
import shutil
import subprocess
import sys
import tarfile
import threading
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from queue import Queue, Empty

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext

# --------------------------------------------------------------------------
# Configuration des dépendances par OS
# --------------------------------------------------------------------------

DEPENDENCIES = {
    "Linux": [
        {"id": "build-essential", "label": "GCC + Make (build-essential)", "apt": "build-essential"},
        {"id": "binutils", "label": "Binutils (as, ld)", "apt": "binutils"},
        {"id": "xorriso", "label": "xorriso (pour grub-mkrescue)", "apt": "xorriso"},
        {"id": "grub-pc-bin", "label": "GRUB (binaires PC BIOS)", "apt": "grub-pc-bin"},
        {"id": "grub-common", "label": "GRUB (commun, fournit grub-mkrescue)", "apt": "grub-common"},
        {"id": "mtools", "label": "mtools (requis par grub-mkrescue)", "apt": "mtools"},
        {"id": "qemu-system-x86", "label": "QEMU x86", "apt": "qemu-system-x86"},
    ],
    "Windows": [
        {
            "id": "make",
            "label": "GNU Make (portable)",
            "type": "zip",
            "url": "https://sourceforge.net/projects/ezwinports/files/make-4.4.1-without-guile-w32-bin.zip/download",
            "bin_subdir": "bin",
        },
        {
            "id": "cross_gcc",
            "label": "Cross-compiler x86_64-elf-gcc + binutils (portable)",
            "type": "github_release",
            "repo": "lordmilko/i686-elf-tools",
            "asset_keywords": ["x86_64-elf", "windows"],
            "bin_subdir": "bin",
        },
        {
            "id": "qemu",
            "label": "QEMU (installeur silencieux, sans admin si hors Program Files)",
            "type": "installer_silent",
            "index_url": "https://qemu.weilnetz.de/w64/",
            "asset_pattern": r"qemu-w64-setup-\d{8}\.exe",
        },
        {
            "id": "wsl_iso_tools",
            "label": "WSL/Debian - outils ISO uniquement (grub-mkrescue, xorriso, mtools)",
            "type": "wsl",
            "note": "Léger : installe WSL/Debian si absent (pas Ubuntu — plus petit, apt "
                    "identique), puis SEULEMENT les paquets nécessaires à la génération de "
                    "l'ISO (make/gcc/QEMU restent natifs Windows, inchangés). Nécessite les "
                    "droits admin pour l'activation initiale de WSL.",
        },
    ],
    "Darwin": [
        {"id": "make", "label": "GNU Make (Homebrew)", "brew": "make"},
        {"id": "qemu", "label": "QEMU (Homebrew)", "brew": "qemu"},
        {
            "id": "cross_gcc",
            "label": "Cross-compiler x86_64-elf-gcc (tap tiers, best-effort)",
            "brew_tap": "messense/macos-cross-toolchains",
            "brew": "x86_64-elf-gcc",
            "note": "Tap non garanti à jour — vérifier manuellement si l'install échoue.",
        },
    ],
}

GITHUB_API = "https://api.github.com/repos/{repo}/releases/latest"

# Dossier système par défaut pour l'installation "propre" (admin) sous Windows
WINDOWS_SYSTEM_INSTALL_DIR = r"C:\Program Files\NovexOS-Toolchain"


# --------------------------------------------------------------------------
# Helpers Windows : élévation UAC + PATH système (HKLM)
# --------------------------------------------------------------------------

def is_windows_admin() -> bool:
    """Retourne True si le process tourne déjà avec des droits admin (Windows uniquement)."""
    if platform.system() != "Windows":
        return False
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def relaunch_as_admin():
    """Relance ce même script avec élévation UAC (Windows uniquement), puis quitte l'instance actuelle."""
    import ctypes
    params = " ".join(f'"{a}"' for a in sys.argv)
    ctypes.windll.shell32.ShellExecuteW(
        None, "runas", sys.executable, params, None, 1
    )


def add_paths_to_system_path(paths):
    """
    Ajoute durablement des dossiers au PATH système (HKLM), sans dupliquer
    les entrées déjà présentes. Nécessite d'être exécuté en admin.
    Puis notifie les autres processus du changement (WM_SETTINGCHANGE).
    """
    import winreg
    import ctypes

    key_path = r"System\CurrentControlSet\Control\Session Manager\Environment"
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_path, 0, winreg.KEY_ALL_ACCESS) as key:
        try:
            current, reg_type = winreg.QueryValueEx(key, "Path")
        except FileNotFoundError:
            current, reg_type = "", winreg.REG_EXPAND_SZ

        existing_entries = [p.strip().lower() for p in current.split(";") if p.strip()]
        new_entries = [str(p) for p in paths if str(p).strip().lower() not in existing_entries]

        if not new_entries:
            return  # rien à ajouter, déjà présent

        updated = current.rstrip(";") + ";" + ";".join(new_entries)
        winreg.SetValueEx(key, "Path", 0, winreg.REG_EXPAND_SZ, updated)

    # Diffuse le changement aux autres processus (Explorer, futurs terminaux, etc.)
    HWND_BROADCAST = 0xFFFF
    WM_SETTINGCHANGE = 0x1A
    SMTO_ABORTIFHUNG = 0x0002
    result = ctypes.c_long()
    ctypes.windll.user32.SendMessageTimeoutW(
        HWND_BROADCAST, WM_SETTINGCHANGE, 0, "Environment",
        SMTO_ABORTIFHUNG, 5000, ctypes.byref(result)
    )


# --------------------------------------------------------------------------
# Logique d'installation (thread séparé, communique via Queue)
# --------------------------------------------------------------------------

class Installer:
    def __init__(self, log_queue: Queue):
        self.q = log_queue

    def log(self, msg):
        self.q.put(("log", msg))

    def progress(self, value):
        self.q.put(("progress", value))

    def done(self, ok, message=""):
        self.q.put(("done", (ok, message)))

    # ---- téléchargement générique -----------------------------------
    def download(self, url, dest_path: Path):
        self.log(f"Téléchargement : {url}")

        def hook(block_num, block_size, total_size):
            if total_size > 0:
                pct = min(100, int(block_num * block_size * 100 / total_size))
                self.progress(pct)

        try:
            urllib.request.urlretrieve(url, dest_path, reporthook=hook)
        except Exception as e:
            raise RuntimeError(f"Échec du téléchargement de {url} : {e}")
        self.log(f"  -> téléchargé dans {dest_path}")

    def github_latest_asset_url(self, repo, keywords):
        self.log(f"Recherche du dernier asset GitHub pour {repo}...")
        req = urllib.request.Request(
            GITHUB_API.format(repo=repo),
            headers={
                "Accept": "application/vnd.github+json",
                "User-Agent": "novexos-deps-installer",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = json.load(resp)
        except urllib.error.HTTPError as e:
            if e.code == 403:
                raise RuntimeError(
                    "Limite de requêtes GitHub atteinte (403, quota anonyme de 60 req/h par IP). "
                    "Réessaie dans quelques minutes, ou télécharge manuellement le fichier "
                    f"x86_64-elf-tools-windows.zip depuis "
                    f"https://github.com/{repo}/releases/latest"
                )
            raise RuntimeError(f"Erreur GitHub API ({e.code}) pour {repo} : {e.reason}")
        assets = data.get("assets", [])
        for asset in assets:
            name = asset["name"].lower()
            if all(k.lower() in name for k in keywords):
                self.log(f"  -> asset trouvé : {asset['name']}")
                return asset["browser_download_url"], asset["name"]
        raise RuntimeError(
            f"Aucun asset correspondant à {keywords} trouvé dans la dernière release de {repo}. "
            f"Va vérifier manuellement sur https://github.com/{repo}/releases"
        )

    def extract_archive(self, archive_path: Path, dest_dir: Path):
        self.log(f"Extraction vers {dest_dir}...")
        dest_dir.mkdir(parents=True, exist_ok=True)
        if archive_path.suffix == ".zip":
            with zipfile.ZipFile(archive_path) as z:
                z.extractall(dest_dir)
        elif archive_path.name.endswith((".tar.gz", ".tgz")):
            with tarfile.open(archive_path, "r:gz") as t:
                t.extractall(dest_dir)
        elif archive_path.name.endswith(".tar.xz"):
            with tarfile.open(archive_path, "r:xz") as t:
                t.extractall(dest_dir)
        else:
            raise RuntimeError(f"Format d'archive non géré : {archive_path.name}")
        self.log("  -> extraction terminée")

    def resolve_latest_from_index(self, index_url, pattern):
        """Récupère la page d'index HTML et trouve le nom de fichier le plus
        récent correspondant au pattern (tri lexicographique = tri par date
        pour un format YYYYMMDD). Évite de coder une URL en dur qui périme."""
        self.log(f"Recherche du dernier installeur sur {index_url}...")
        req = urllib.request.Request(index_url, headers={"User-Agent": "novexos-deps-installer"})
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                html = resp.read().decode("utf-8", errors="ignore")
        except Exception as e:
            raise RuntimeError(f"Impossible de lire {index_url} : {e}")

        matches = sorted(set(re.findall(pattern, html)))
        if not matches:
            raise RuntimeError(
                f"Aucun fichier correspondant à {pattern!r} trouvé sur {index_url}. "
                "Va vérifier manuellement sur cette page."
            )
        latest = matches[-1]
        url = index_url.rstrip("/") + "/" + latest
        self.log(f"  -> dernier trouvé : {latest}")
        return url

    # ---- Windows : mode portable (sans admin) ou mode système (admin) ----
    def install_windows(self, deps, install_dir: Path, system_mode: bool = False):
        install_dir.mkdir(parents=True, exist_ok=True)
        if system_mode:
            self.log(f"Mode : installation propre (admin), dossier système {install_dir}")
        else:
            self.log(f"Mode : installation portable (sans admin), dossier {install_dir}")
        bin_paths = []
        for dep in deps:
            if dep.get("type") == "wsl":
                self.log(f"\n=== {dep['label']} ===")
                self.install_wsl_iso_tools(dep)
                continue  # pas de dossier bin/ à ajouter au PATH pour celui-ci
            self.log(f"\n=== {dep['label']} ===")
            dep_dir = install_dir / dep["id"]

            if dep["type"] == "zip":
                archive = install_dir / f"{dep['id']}.zip"
                self.download(dep["url"], archive)
                self.extract_archive(archive, dep_dir)
                archive.unlink(missing_ok=True)

            elif dep["type"] == "github_release":
                url, name = self.github_latest_asset_url(dep["repo"], dep["asset_keywords"])
                archive = install_dir / name
                self.download(url, archive)
                self.extract_archive(archive, dep_dir)
                archive.unlink(missing_ok=True)

            elif dep["type"] == "installer_silent":
                if dep.get("note"):
                    self.log(f"  Note : {dep['note']}")
                if "index_url" in dep and "asset_pattern" in dep:
                    download_url = self.resolve_latest_from_index(dep["index_url"], dep["asset_pattern"])
                else:
                    download_url = dep["url"]
                installer_path = install_dir / "qemu-setup.exe"
                self.download(download_url, installer_path)
                dep_dir.mkdir(parents=True, exist_ok=True)
                self.log("  Lancement de l'installeur QEMU en mode silencieux "
                          "(pas d'admin requis car installé hors Program Files)...")
                # IMPORTANT (NSIS) : le switch /D= n'accepte AUCUN guillemet autour du
                # chemin, même s'il contient des espaces (ex: "C:\Program Files\...").
                # En passant une liste à subprocess.run, Python quote automatiquement
                # tout argument contenant un espace -> NSIS reçoit alors /D="C:\Program
                # Files\..." avec des guillemets, ce qu'il refuse silencieusement (aucune
                # erreur, mais installation dans le dossier par défaut ou nulle part).
                # On construit donc la commande en une seule chaîne, avec /D= toujours
                # en dernier et jamais quoté, et on l'exécute via shell=True.
                dep_dir_str = str(dep_dir).rstrip("\\")
                cmd = f'"{installer_path}" /S /D={dep_dir_str}'
                self.log(f"  Commande : {cmd}")
                try:
                    subprocess.run(
                        cmd,
                        shell=True,
                        check=True,
                        timeout=300,
                    )
                except Exception as e:
                    raise RuntimeError(f"L'installeur QEMU a échoué : {e}")

                if not any(dep_dir.glob("*.exe")):
                    raise RuntimeError(
                        f"L'installeur QEMU s'est terminé sans erreur mais aucun .exe "
                        f"n'a été trouvé dans {dep_dir}. Vérifie que le chemin d'installation "
                        f"ne contient pas de guillemets mal placés, ou installe QEMU manuellement "
                        f"dans ce dossier avec l'option /D."
                    )
                installer_path.unlink(missing_ok=True)

            # trouve le sous-dossier bin/ réel (l'archive peut extraire dans un sous-dossier imbriqué)
            candidate = self._find_bin_dir(dep_dir)
            if candidate:
                bin_paths.append(candidate)
                self.log(f"  -> ajouté au PATH : {candidate}")
            else:
                self.log(f"  ATTENTION : dossier bin/ introuvable sous {dep_dir}, "
                          f"vérifie manuellement le contenu.")

        if system_mode and bin_paths:
            self.log("\nÉcriture des chemins dans le PATH système (HKLM)...")
            try:
                add_paths_to_system_path(bin_paths)
                self.log("  -> PATH système mis à jour. Ouvre un NOUVEAU terminal pour "
                          "que le changement soit pris en compte (pas besoin de redémarrer Windows).")
            except Exception as e:
                raise RuntimeError(f"Échec de l'écriture du PATH système : {e}")

        return bin_paths

    @staticmethod
    def _find_bin_dir(root: Path, max_depth=4):
        """Cherche le dossier le plus proche de la racine contenant des .exe.
        Certains installeurs rangent tout dans bin/ (make, cross-compiler),
        d'autres (QEMU) déposent les .exe directement à la racine — on
        vérifie donc la racine elle-même avant de descendre dans l'arbre,
        en largeur d'abord pour privilégier le résultat le plus proche."""
        if not root.exists():
            return None
        queue = deque([(root, 0)])
        while queue:
            current, depth = queue.popleft()
            if any(current.glob("*.exe")):
                return current
            if depth < max_depth:
                try:
                    for child in current.iterdir():
                        if child.is_dir():
                            queue.append((child, depth + 1))
                except OSError:
                    pass
        return None

    # ---- Windows : WSL léger, uniquement pour les outils de génération d'ISO ----
    def install_wsl_iso_tools(self, dep):
        if dep.get("note"):
            self.log(f"  Note : {dep['note']}")

        if shutil.which("wsl") is None:
            raise RuntimeError(
                "La commande 'wsl' est introuvable. Ton Windows est probablement trop "
                "ancien pour WSL2, ou la fonctionnalité n'est pas activable ici."
            )

        # 1. Vérifie si une distro WSL existe déjà
        self.log("  Vérification des distributions WSL existantes...")
        try:
            result = subprocess.run(
                ["wsl", "-l", "-q"], capture_output=True, timeout=30
            )
            # wsl -l -q sort du texte en UTF-16LE sous Windows
            distros = [
                d.strip() for d in result.stdout.decode("utf-16-le", errors="ignore").splitlines()
                if d.strip()
            ]
        except Exception:
            distros = []

        if not distros:
            self.log("  Aucune distro WSL détectée. Installation de Debian "
                      "(léger, apt identique à Ubuntu — nécessite les droits admin)...")
            try:
                subprocess.run(
                    ["wsl", "--install", "-d", "Debian", "--no-launch"],
                    check=True, timeout=1800,
                )
            except subprocess.CalledProcessError as e:
                raise RuntimeError(
                    f"L'installation de WSL/Debian a échoué (code {e.returncode}). "
                    "Relance l'appli en tant qu'administrateur, ou installe manuellement "
                    "avec : wsl --install -d Debian"
                )
            self.log(
                "  -> WSL/Debian installé. IMPORTANT : Windows peut demander un redémarrage "
                "pour activer la virtualisation. Si c'est le cas, redémarre puis relance "
                "l'installateur pour terminer (création du compte Linux au premier lancement "
                "de 'wsl', puis les paquets ci-dessous)."
            )
            self.log(
                "  Lance 'wsl' une fois manuellement pour finaliser la création du compte "
                "utilisateur Linux (obligatoire au premier démarrage), puis relance "
                "l'installateur pour poursuivre avec les paquets apt."
            )
            return  # rien de plus à faire tant que le compte Linux n'est pas créé

        self.log(f"  Distro(s) WSL détectée(s) : {', '.join(distros)}")

        # 2. Installe uniquement les paquets nécessaires à l'ISO (léger, pas de
        #    build-essential/qemu : ces outils restent natifs Windows).
        packages = ["xorriso", "grub-common", "grub-pc-bin", "mtools"]
        self.log(f"  Installation des paquets (via -u root, sans mot de passe) : {', '.join(packages)}")
        try:
            subprocess.run(
                ["wsl", "-u", "root", "apt-get", "update"],
                check=True, timeout=300,
            )
            subprocess.run(
                ["wsl", "-u", "root", "apt-get", "install", "-y"] + packages,
                check=True, timeout=600,
            )
        except subprocess.CalledProcessError as e:
            raise RuntimeError(
                f"Échec de l'installation des paquets dans WSL (code {e.returncode}). "
                "Vérifie qu'une distro Debian/Ubuntu est bien installée (les commandes "
                "apt-get ne fonctionnent pas sur toutes les distros WSL, ex: Alpine)."
            )

        self.log("  -> Outils ISO installés dans WSL. 'make iso' fonctionnera depuis Windows.")

    # ---- Linux : apt-get (nécessite sudo) -----------------------------
    def install_linux(self, deps, sudo_password: str):
        packages = [d["apt"] for d in deps]
        self.log(f"Paquets à installer via apt : {', '.join(packages)}")

        def run_sudo(cmd):
            full_cmd = ["sudo", "-S"] + cmd
            proc = subprocess.Popen(
                full_cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            proc.stdin.write(sudo_password + "\n")
            proc.stdin.flush()
            for line in proc.stdout:
                self.log(line.rstrip())
            proc.wait()
            if proc.returncode != 0:
                raise RuntimeError(f"Commande échouée (code {proc.returncode}) : {' '.join(cmd)}")

        run_sudo(["apt-get", "update"])
        run_sudo(["apt-get", "install", "-y"] + packages)

    # ---- macOS : Homebrew ----------------------------------------------
    def install_macos(self, deps):
        if shutil.which("brew") is None:
            raise RuntimeError(
                "Homebrew n'est pas installé. Installe-le d'abord depuis https://brew.sh "
                "(nécessite une exécution manuelle du script officiel)."
            )
        for dep in deps:
            self.log(f"\n=== {dep['label']} ===")
            if dep.get("note"):
                self.log(f"  Note : {dep['note']}")
            if "brew_tap" in dep:
                self._run(["brew", "tap", dep["brew_tap"]])
            self._run(["brew", "install", dep["brew"]])

    def _run(self, cmd):
        self.log(f"$ {' '.join(cmd)}")
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in proc.stdout:
            self.log(line.rstrip())
        proc.wait()
        if proc.returncode != 0:
            raise RuntimeError(f"Commande échouée (code {proc.returncode}) : {' '.join(cmd)}")


# --------------------------------------------------------------------------
# Génération du script d'environnement (PATH pour la session de build)
# --------------------------------------------------------------------------

def write_env_script(os_name, install_dir: Path, bin_paths):
    if os_name == "Windows":
        script = install_dir / "env.bat"
        path_additions = ";".join(str(p) for p in bin_paths)
        script.write_text(
            "@echo off\r\n"
            f'set "PATH={path_additions};%PATH%"\r\n'
            'echo Environnement NovexOS pret. Tu peux maintenant lancer : make build\r\n'
        )
    else:
        script = install_dir / "env.sh"
        path_additions = ":".join(str(p) for p in bin_paths)
        script.write_text(
            "#!/usr/bin/env bash\n"
            f'export PATH="{path_additions}:$PATH"\n'
            'echo "Environnement NovexOS pret. Tu peux maintenant lancer : make build"\n'
        )
        script.chmod(0o755)
    return script


# --------------------------------------------------------------------------
# GUI
# --------------------------------------------------------------------------

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("NovexOS - Installateur de dépendances de build")
        self.geometry("640x600")
        self.resizable(True, True)

        self.os_name = platform.system()  # "Linux", "Windows", "Darwin"
        self.deps = DEPENDENCIES.get(self.os_name, [])
        self.dep_vars = {}
        self.log_queue = Queue()

        self._build_ui()
        self._poll_queue()

    # ---- UI ----------------------------------------------------------
    def _build_ui(self):
        pad = {"padx": 10, "pady": 6}

        header = ttk.Label(
            self, text=f"OS détecté : {self.os_name}", font=("TkDefaultFont", 12, "bold")
        )
        header.pack(anchor="w", **pad)

        if self.os_name not in DEPENDENCIES:
            ttk.Label(
                self,
                text="OS non reconnu — aucune dépendance connue pour cette plateforme.",
                foreground="red",
            ).pack(**pad)
            return

        # --- Mode d'installation (Windows uniquement : admin/système vs portable) ---
        self.install_mode_var = tk.StringVar(value="admin" if self.os_name == "Windows" else "n/a")

        if self.os_name == "Windows":
            mode_frame = ttk.LabelFrame(self, text="Mode d'installation")
            mode_frame.pack(fill="x", **pad)

            ttk.Radiobutton(
                mode_frame,
                text="Propre (admin) — installe dans les dossiers système, PATH permanent",
                variable=self.install_mode_var,
                value="admin",
                command=self._on_mode_change,
            ).pack(anchor="w", padx=8, pady=2)
            ttk.Radiobutton(
                mode_frame,
                text="Portable (sans admin) — dossier au choix, PATH valable pour la session uniquement",
                variable=self.install_mode_var,
                value="portable",
                command=self._on_mode_change,
            ).pack(anchor="w", padx=8, pady=2)

            admin_status = "admin ✔" if is_windows_admin() else "PAS admin — une fenêtre UAC sera demandée en mode Propre"
            ttk.Label(mode_frame, text=f"Statut actuel du process : {admin_status}", foreground="gray").pack(
                anchor="w", padx=8, pady=(0, 4)
            )

        # --- Dossier d'installation ---
        path_frame = ttk.LabelFrame(self, text="Dossier d'installation")
        path_frame.pack(fill="x", **pad)

        default_dir = self._default_install_dir()
        self.path_var = tk.StringVar(value=str(default_dir))
        entry = ttk.Entry(path_frame, textvariable=self.path_var, width=60)
        entry.pack(side="left", padx=8, pady=8, fill="x", expand=True)
        ttk.Button(path_frame, text="Parcourir...", command=self._browse).pack(
            side="left", padx=8
        )

        # --- Options ---
        opt_frame = ttk.LabelFrame(self, text="Options")
        opt_frame.pack(fill="x", **pad)

        self.noadmin_var = tk.BooleanVar(value=(self.os_name != "Linux"))
        noadmin_cb = ttk.Checkbutton(
            opt_frame,
            text="Installation sans droits admin (binaires portables uniquement)",
            variable=self.noadmin_var,
        )
        if self.os_name != "Windows":
            noadmin_cb.pack(anchor="w", padx=8, pady=4)
        if self.os_name == "Linux":
            noadmin_cb.state(["disabled"])
            ttk.Label(
                opt_frame,
                text="apt-get nécessite les droits root sous Linux — mot de passe sudo demandé ci-dessous.",
                foreground="gray",
            ).pack(anchor="w", padx=8)

        # --- Mot de passe sudo (Linux uniquement) ---
        self.sudo_var = tk.StringVar()
        if self.os_name == "Linux":
            sudo_frame = ttk.Frame(opt_frame)
            sudo_frame.pack(fill="x", padx=8, pady=4)
            ttk.Label(sudo_frame, text="Mot de passe sudo :").pack(side="left")
            ttk.Entry(sudo_frame, textvariable=self.sudo_var, show="*", width=30).pack(
                side="left", padx=8
            )

        # --- Liste des dépendances ---
        dep_frame = ttk.LabelFrame(self, text="Dépendances à installer")
        dep_frame.pack(fill="both", expand=False, **pad)
        for dep in self.deps:
            var = tk.BooleanVar(value=True)
            self.dep_vars[dep["id"]] = var
            ttk.Checkbutton(dep_frame, text=dep["label"], variable=var).pack(
                anchor="w", padx=8, pady=2
            )

        # --- Bouton d'installation ---
        self.install_btn = ttk.Button(
            self, text="Installer les dépendances", command=self._on_install
        )
        self.install_btn.pack(**pad)

        # --- Barre de progression ---
        self.progress = ttk.Progressbar(self, mode="determinate", maximum=100)
        self.progress.pack(fill="x", **pad)

        # --- Log ---
        log_frame = ttk.LabelFrame(self, text="Journal")
        log_frame.pack(fill="both", expand=True, **pad)
        self.log_text = scrolledtext.ScrolledText(log_frame, height=14, state="disabled")
        self.log_text.pack(fill="both", expand=True, padx=4, pady=4)

    def _default_install_dir(self) -> Path:
        if self.os_name == "Windows":
            if self.install_mode_var.get() == "admin":
                return Path(WINDOWS_SYSTEM_INSTALL_DIR)
            return Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "NovexOS-Toolchain"
        elif self.os_name == "Darwin":
            return Path.home() / "novexos-toolchain"
        else:
            return Path("/usr")  # apt installe dans les emplacements système standards

    def _on_mode_change(self):
        self.path_var.set(str(self._default_install_dir()))

    def _browse(self):
        chosen = filedialog.askdirectory(initialdir=self.path_var.get())
        if chosen:
            self.path_var.set(chosen)

    # ---- lancement de l'installation dans un thread -------------------
    def _on_install(self):
        selected = [d for d in self.deps if self.dep_vars[d["id"]].get()]
        if not selected:
            messagebox.showwarning("Rien à faire", "Sélectionne au moins une dépendance.")
            return

        if self.os_name == "Linux" and not self.sudo_var.get():
            messagebox.showwarning("Mot de passe requis", "Entre ton mot de passe sudo.")
            return

        if (
            self.os_name == "Windows"
            and self.install_mode_var.get() == "admin"
            and not is_windows_admin()
        ):
            relaunch = messagebox.askyesno(
                "Droits administrateur requis",
                "Le mode Propre installe dans les dossiers système et modifie le PATH "
                "système : ça nécessite les droits admin.\n\n"
                "Relancer l'application avec élévation (UAC) maintenant ?",
            )
            if relaunch:
                relaunch_as_admin()
                self.destroy()
            return

        self.install_btn.state(["disabled"])
        self.progress["value"] = 0
        self._clear_log()

        thread = threading.Thread(
            target=self._run_install, args=(selected,), daemon=True
        )
        thread.start()

    def _run_install(self, selected):
        installer = Installer(self.log_queue)
        install_dir = Path(self.path_var.get())
        try:
            if self.os_name == "Windows":
                system_mode = self.install_mode_var.get() == "admin"
                bin_paths = installer.install_windows(selected, install_dir, system_mode=system_mode)
                if system_mode:
                    installer.log("\nInstallation terminée dans les dossiers système, "
                                   "PATH mis à jour de façon permanente.")
                    installer.log("Ouvre un NOUVEAU terminal (cmd/PowerShell) puis lance directement : "
                                   "make build")
                else:
                    script = write_env_script("Windows", install_dir, bin_paths)
                    installer.log(f"\nScript d'environnement généré : {script}")
                    installer.log(f"Avant de builder, lance : {script}")
                    installer.log("Puis dans la même fenêtre : make build "
                                   "(le Makefile détectera x86_64-elf-gcc automatiquement)")

            elif self.os_name == "Linux":
                installer.install_linux(selected, self.sudo_var.get())
                installer.log("\nDépendances installées via apt. Rien d'autre à faire, "
                               "make build fonctionnera directement.")

            elif self.os_name == "Darwin":
                installer.install_macos(selected)
                bin_paths = [Path("/opt/homebrew/bin"), Path("/usr/local/bin")]
                script = write_env_script("Darwin", install_dir, bin_paths)
                installer.log(f"\nScript d'environnement généré : {script}")
                installer.log(f"Avant de builder : source {script}")

            installer.done(True, "Installation terminée avec succès.")
        except Exception as e:
            installer.done(False, str(e))

    # ---- communication thread -> UI ------------------------------------
    def _poll_queue(self):
        try:
            while True:
                kind, payload = self.log_queue.get_nowait()
                if kind == "log":
                    self._append_log(payload)
                elif kind == "progress":
                    self.progress["value"] = payload
                elif kind == "done":
                    ok, message = payload
                    self.install_btn.state(["!disabled"])
                    if ok:
                        messagebox.showinfo("Terminé", message)
                    else:
                        messagebox.showerror("Erreur", message)
                        self._append_log(f"ERREUR : {message}")
        except Empty:
            pass
        self.after(100, self._poll_queue)

    def _append_log(self, text):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", text + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _clear_log(self):
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")


if __name__ == "__main__":
    if sys.version_info < (3, 8):
        print("Python 3.8+ requis.")
        sys.exit(1)
    App().mainloop()