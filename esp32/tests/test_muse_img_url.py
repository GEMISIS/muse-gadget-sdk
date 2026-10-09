# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""A web image's smaller copy by its CDN's URL (muse_img_url.h): ~640 px
asked of the CDNs that make one, nothing changed for anything else."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MuseImgUrlTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "muse_img_url"
        cc = shlex.split(os.environ.get("CC", "cc"))
        compiled = subprocess.run(
            [*cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(ROOT / "components/muse"),
             str(ROOT / "tests/muse_img_url_harness.c"), "-o", str(cls.binary)],
            capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def smaller(self, *urls, **env):
        ran = subprocess.run([str(self.binary), *urls], capture_output=True, text=True,
                             env={**os.environ, **{k.upper(): str(v) for k, v in env.items()},
                                  "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
                                  "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(ran.returncode, 0, ran.stderr)
        out = ran.stdout.splitlines()
        return out[0] if len(urls) == 1 else out

    def check(self, url, want):
        self.assertEqual(self.smaller(url), want, url)

    def test_wordpress(self):
        self.check("https://i0.wp.com/example.com/wp-content/uploads/2024/01/cat.jpg?resize=1920%2C1080&ssl=1",
                   "https://i0.wp.com/example.com/wp-content/uploads/2024/01/cat.jpg?ssl=1&w=640")
        self.check("https://i2.wp.com/example.com/a.jpg", "https://i2.wp.com/example.com/a.jpg?w=640")
        self.check("https://blog.example.org/wp-content/uploads/2023/05/dog.jpeg",
                   "https://blog.example.org/wp-content/uploads/2023/05/dog.jpeg?w=640")
        self.check("https://site.files.wordpress.com/2020/01/x.png?w=2048&h=1024",
                   "https://site.files.wordpress.com/2020/01/x.png?w=640")
        self.check("https://i1.wp.com/example.com/a.jpg?w=300", "-")   # small already

    def test_wikimedia(self):
        self.check("https://upload.wikimedia.org/wikipedia/commons/a/a7/Red_Panda.jpg",
                   "https://upload.wikimedia.org/wikipedia/commons/thumb/a/a7/Red_Panda.jpg/500px-Red_Panda.jpg")
        self.check("https://upload.wikimedia.org/wikipedia/en/4/4a/Logo.svg",
                   "https://upload.wikimedia.org/wikipedia/en/thumb/4/4a/Logo.svg/500px-Logo.svg.png")
        self.check("https://upload.wikimedia.org/wikipedia/commons/thumb/a/a7/Red_Panda.jpg/1920px-Red_Panda.jpg",
                   "https://upload.wikimedia.org/wikipedia/commons/thumb/a/a7/Red_Panda.jpg/500px-Red_Panda.jpg")
        self.check("https://upload.wikimedia.org/wikipedia/commons/thumb/a/a7/Red_Panda.jpg/330px-Red_Panda.jpg", "-")
        self.check("https://upload.wikimedia.org/wikipedia/commons/a/a7/Scan.tif", "-")
        self.check("https://upload.wikimedia.org/wikipedia/commons/a7/Odd.jpg", "-")

    def test_unsplash_and_imgix(self):
        self.check("https://images.unsplash.com/photo-1518791841217-8f162f1e1131"
                   "?ixlib=rb-4.0.3&auto=format&fit=crop&w=3000&q=80",
                   "https://images.unsplash.com/photo-1518791841217-8f162f1e1131"
                   "?ixlib=rb-4.0.3&fit=crop&w=640&fm=jpg&q=70")
        self.check("https://acme.imgix.net/p/cat.png", "https://acme.imgix.net/p/cat.png?w=640&fm=jpg&q=70")

    def test_cloudinary(self):
        self.check("https://res.cloudinary.com/demo/image/upload/v1312461204/sample.jpg",
                   "https://res.cloudinary.com/demo/image/upload/w_640,c_limit,f_jpg,q_auto/v1312461204/sample.jpg")
        self.check("https://res.cloudinary.com/demo/image/upload/c_fill,h_2000/v1/folder/sample.jpg",
                   "https://res.cloudinary.com/demo/image/upload/c_fill,h_2000/w_640,c_limit,f_jpg,q_auto/v1/folder/"
                   "sample.jpg")
        self.check("https://res.cloudinary.com/demo/image/upload/sample.jpg",
                   "https://res.cloudinary.com/demo/image/upload/w_640,c_limit,f_jpg,q_auto/sample.jpg")
        self.check("https://res.cloudinary.com/demo/image/upload/w_640,c_limit,f_jpg,q_auto/sample.jpg", "-")
        self.check("https://res.cloudinary.com/demo/video/upload/sample.mp4", "-")

    def test_google(self):
        self.check("https://lh3.googleusercontent.com/abcDEF123=s1600",
                   "https://lh3.googleusercontent.com/abcDEF123=w640")
        self.check("https://lh5.googleusercontent.com/p/AF1QipN=w2000-h1500-k-no",
                   "https://lh5.googleusercontent.com/p/AF1QipN=w640")
        self.check("https://lh3.googleusercontent.com/abcDEF123", "https://lh3.googleusercontent.com/abcDEF123=w640")
        self.check("https://blogger.googleusercontent.com/img/b/R29vZ2xl/AVvXsE/s1600/cat.jpg",
                   "https://blogger.googleusercontent.com/img/b/R29vZ2xl/AVvXsE/w640/cat.jpg")
        self.check("https://lh3.googleusercontent.com/abc=s200", "-")
        self.check("https://docs.googleusercontent.com/abc", "-")

    def test_shopify_squarespace_pinterest(self):
        self.check("https://cdn.shopify.com/s/files/1/0001/products/shoe.jpg?v=1700000000",
                   "https://cdn.shopify.com/s/files/1/0001/products/shoe.jpg?v=1700000000&width=640")
        self.check("https://shop.example.com/cdn/shop/files/hat.png?v=1&width=3000",
                   "https://shop.example.com/cdn/shop/files/hat.png?v=1&width=640")
        self.check("https://images.squarespace-cdn.com/content/v1/abc/photo.jpg",
                   "https://images.squarespace-cdn.com/content/v1/abc/photo.jpg?format=750w")
        self.check("https://images.squarespace-cdn.com/content/v1/abc/photo.jpg?format=2500w",
                   "https://images.squarespace-cdn.com/content/v1/abc/photo.jpg?format=750w")
        self.check("https://i.pinimg.com/originals/ab/cd/ef/abcdef.jpg", "https://i.pinimg.com/564x/ab/cd/ef/abcdef.jpg")
        self.check("https://i.pinimg.com/736x/ab/cd/ef/abcdef.jpg", "https://i.pinimg.com/564x/ab/cd/ef/abcdef.jpg")
        self.check("https://i.pinimg.com/236x/ab/cd/ef/abcdef.jpg", "-")

    def test_medium_amazon_fandom_imgur(self):
        self.check("https://miro.medium.com/v2/resize:fit:1400/1*abc.jpeg",
                   "https://miro.medium.com/v2/resize:fit:640/1*abc.jpeg")
        self.check("https://cdn-images-1.medium.com/max/2000/1*abc.png",
                   "https://cdn-images-1.medium.com/max/640/1*abc.png")
        self.check("https://m.media-amazon.com/images/I/71abcDEF12L._AC_SL1500_.jpg",
                   "https://m.media-amazon.com/images/I/71abcDEF12L._SL640_.jpg")
        self.check("https://m.media-amazon.com/images/I/71abcDEF12L.jpg",
                   "https://m.media-amazon.com/images/I/71abcDEF12L._SL640_.jpg")
        self.check("https://m.media-amazon.com/images/I/71abcDEF12L._SL640_.jpg", "-")
        self.check("https://static.wikia.nocookie.net/starwars/images/a/ab/Yoda.png/revision/latest?cb=2020",
                   "https://static.wikia.nocookie.net/starwars/images/a/ab/Yoda.png/revision/latest/"
                   "scale-to-width-down/640?cb=2020")
        self.check("https://static.wikia.nocookie.net/w/images/a/ab/Y.png/revision/latest/scale-to-width-down/1200?cb=1",
                   "https://static.wikia.nocookie.net/w/images/a/ab/Y.png/revision/latest/scale-to-width-down/640?cb=1")
        self.check("https://i.imgur.com/AbC12xY.jpg", "https://i.imgur.com/AbC12xYl.jpg")
        self.check("https://i.imgur.com/AbC12xY.gif", "-")

    def test_others_left_alone(self):
        self.assertEqual(self.smaller("https://example.com/photo.jpg", "http://x", "ftp://upload.wikimedia.org/a.jpg",
                                      "not a url", "https://i.imgur.com/AbC12xYl.jpg"), ["-"] * 5)

    def test_too_long_for_the_buffer(self):
        url = "https://images.unsplash.com/photo-1?" + "a=b&" * 10 + "w=3000"
        self.assertEqual(self.smaller(url, cap=40), "-")
        self.assertTrue(self.smaller(url).endswith("w=640&fm=jpg&q=70"))
        self.assertEqual(self.smaller("https://i0.wp.com/" + "x" * 700 + ".jpg"), "-")


if __name__ == "__main__":
    unittest.main()
