import os
import shutil
from io import BytesIO
from urllib.request import urlopen
from zipfile import ZipFile


def download_and_unzip(url, extract_to="."):
    response = urlopen(url)
    archive = ZipFile(BytesIO(response.read()))
    archive.extractall(path=extract_to)


print("Get boost release headers")

boost_dir = "lib/boost_1_86_0"
if not os.path.exists(boost_dir):
    os.makedirs("lib", exist_ok=True)
    download_and_unzip(
        "https://archives.boost.io/release/1.86.0/source/boost_1_86_0.zip",
        extract_to="lib/",
    )
    for name in ["libs", "more", "status", "tools", "doc"]:
        path = os.path.join(boost_dir, name)
        if os.path.exists(path):
            shutil.rmtree(path)
