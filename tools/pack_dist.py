"""Write the player package archives with Unix permissions, so the Linux scripts stay executable after unpacking
(Compress-Archive cannot store them):
  dist/<name>.zip            the whole package
  dist/<name>-linux.tar.gz   the linux folder plus HOW TO TEST.txt and VERSION.txt, for Linux and Steam Deck
Usage (make_package.ps1 runs it):  python -I tools/pack_dist.py dist/<name>
"""
import os
import sys
import tarfile
import zipfile

EXTRA_FOR_LINUX = ("HOW TO TEST.txt", "VERSION.txt")


def mode(rel):
    return 0o755 if rel.endswith(".sh") else 0o644


def main():
    src = os.path.abspath(sys.argv[1])
    name = os.path.basename(src)
    files = []
    for root, dirs, names in os.walk(src):
        dirs.sort()
        for n in sorted(names):
            full = os.path.join(root, n)
            files.append((full, os.path.relpath(full, src).replace(os.sep, "/")))

    with zipfile.ZipFile(src + ".zip", "w", zipfile.ZIP_DEFLATED) as z:
        for full, rel in files:
            info = zipfile.ZipInfo.from_file(full, rel)
            info.create_system = 3  # Unix: unzip and file managers then apply the permission bits below
            info.external_attr = (0o100000 | mode(rel)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(full, "rb") as f:
                z.writestr(info, f.read())

    linux = [(full, rel[len("linux/"):]) for full, rel in files if rel.startswith("linux/")]
    linux += [(full, rel) for full, rel in files if rel in EXTRA_FOR_LINUX]
    if linux:
        with tarfile.open(os.path.join(os.path.dirname(src), name + "-linux.tar.gz"), "w:gz") as t:
            for full, rel in linux:
                info = t.gettarinfo(full, arcname=name + "-linux/" + rel)
                info.mode = mode(rel)
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                with open(full, "rb") as f:
                    t.addfile(info, f)


if __name__ == "__main__":
    main()
