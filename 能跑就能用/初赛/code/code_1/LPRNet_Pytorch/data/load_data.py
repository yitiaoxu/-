from torch.utils.data import *
from imutils import paths
import numpy as np
import random
import cv2
import os

CHARS = ['京', '沪', '津', '渝', '冀', '晋', '蒙', '辽', '吉', '黑',
         '苏', '浙', '皖', '闽', '赣', '鲁', '豫', '鄂', '湘', '粤',
         '桂', '琼', '川', '贵', '云', '藏', '陕', '甘', '青', '宁',
         '新',
         '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
         'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K',
         'L', 'M', 'N', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
         'W', 'X', 'Y', 'Z', 'I', 'O',
         # CBLPRD_330K 中额外出现的字符
         '学', '挂', '港', '澳', '领', '使', '临',
         # CTC blank
         '-'
         ]

CHARS_DICT = {char:i for i, char in enumerate(CHARS)}


def imread_unicode(path):
    # Windows 下 cv2.imread 对中文路径不稳定，改用 imdecode 兼容中文文件名
    data = np.fromfile(path, dtype=np.uint8)
    if data.size == 0:
        return None
    return cv2.imdecode(data, cv2.IMREAD_COLOR)


def augment(img):
    # 亮度/对比度抖动
    if random.random() < 0.5:
        alpha = random.uniform(0.7, 1.3)  # contrast
        beta = random.uniform(-30, 30)    # brightness
        img = np.clip(alpha * img + beta, 0, 255).astype(np.uint8)

    # 小角度旋转
    if random.random() < 0.3:
        h, w = img.shape[:2]
        angle = random.uniform(-5, 5)
        m = cv2.getRotationMatrix2D((w / 2, h / 2), angle, 1.0)
        img = cv2.warpAffine(img, m, (w, h), borderMode=cv2.BORDER_REPLICATE)

    # 运动模糊
    if random.random() < 0.15:
        k = random.choice([3, 5])
        kernel = np.zeros((k, k), dtype=np.float32)
        kernel[k // 2, :] = 1.0 / k
        img = cv2.filter2D(img, -1, kernel)

    # 高斯噪声
    if random.random() < 0.2:
        noise = np.random.randn(*img.shape) * random.uniform(3, 10)
        img = np.clip(img + noise, 0, 255).astype(np.uint8)

    # 随机遮挡（模拟污损）
    if random.random() < 0.2:
        h, w = img.shape[:2]
        ew, eh = random.randint(3, 10), random.randint(3, 8)
        ex = random.randint(0, max(0, w - ew))
        ey = random.randint(0, max(0, h - eh))
        img[ey:ey + eh, ex:ex + ew] = random.randint(0, 255)

    return img


class LPRDataLoader(Dataset):
    def __init__(self, img_dir, imgSize, lpr_max_len, PreprocFun=None, use_augment=False):
        self.img_dir = img_dir
        self.use_augment = use_augment
        self.samples = []
        for i in range(len(img_dir)):
            src = img_dir[i]
            if src.lower().endswith('.txt'):
                self.samples += self._load_samples_from_txt(src)
            else:
                for el in paths.list_images(src):
                    basename = os.path.basename(el)
                    imgname, _ = os.path.splitext(basename)
                    imgname = imgname.split("-")[0].split("_")[0]
                    self.samples.append((el, imgname))
        random.shuffle(self.samples)
        self.img_size = imgSize
        self.lpr_max_len = lpr_max_len
        if PreprocFun is not None:
            self.PreprocFun = PreprocFun
        else:
            self.PreprocFun = self.transform

    def __len__(self):
        return len(self.samples)

    def _load_samples_from_txt(self, txt_path):
        samples = []
        txt_dir = os.path.dirname(txt_path)
        with open(txt_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split()
                if len(parts) < 2:
                    continue
                rel_img_path, plate_text = parts[0], parts[1]
                # 兼容相对路径：相对于 txt 所在目录拼接
                abs_img_path = os.path.normpath(os.path.join(txt_dir, rel_img_path))
                samples.append((abs_img_path, plate_text))
        return samples

    def __getitem__(self, index):
        filename, plate_text = self.samples[index]
        Image = imread_unicode(filename)
        if Image is None:
            # 兜底再试一次标准接口，便于兼容非 Windows 环境
            Image = cv2.imread(filename)
        if Image is None:
            raise RuntimeError("Failed to read image: {}".format(filename))
        height, width, _ = Image.shape
        if height != self.img_size[1] or width != self.img_size[0]:
            Image = cv2.resize(Image, self.img_size)
        if self.use_augment:
            Image = augment(Image)
        Image = self.PreprocFun(Image)

        label = list()
        for c in plate_text:
            # one_hot_base = np.zeros(len(CHARS))
            # one_hot_base[CHARS_DICT[c]] = 1
            if c not in CHARS_DICT:
                raise KeyError("Unknown char '{}' in label '{}' from file '{}'".format(c, plate_text, filename))
            label.append(CHARS_DICT[c])

        # 原仓库对 8 位车牌做了特定数据集规则校验（D/F 位约束），
        # 但 CBLPRD_330K 包含更丰富车牌类型，不能用该规则强制中断训练。
        # 这里保留接口但不再阻断样本加载。

        return Image, label, len(label)

    def transform(self, img):
        img = img.astype('float32')
        img -= 127.5
        img *= 0.0078125
        img = np.transpose(img, (2, 0, 1))

        return img

    def check(self, label):
        if label[2] != CHARS_DICT['D'] and label[2] != CHARS_DICT['F'] \
                and label[-1] != CHARS_DICT['D'] and label[-1] != CHARS_DICT['F']:
            print("Error label, Please check!")
            return False
        else:
            return True
