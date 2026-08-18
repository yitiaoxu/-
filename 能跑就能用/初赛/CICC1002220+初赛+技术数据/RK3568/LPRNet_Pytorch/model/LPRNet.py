import torch.nn as nn
import torch

class small_basic_block(nn.Module):
    def __init__(self, ch_in, ch_out):
        super(small_basic_block, self).__init__()
        mid = ch_out // 4
        self.conv1 = nn.Conv2d(ch_in, mid, kernel_size=1, bias=False)
        self.bn1 = nn.BatchNorm2d(mid)
        self.conv2 = nn.Conv2d(mid, mid, kernel_size=(3, 1), padding=(1, 0), bias=False)
        self.bn2 = nn.BatchNorm2d(mid)
        self.conv3 = nn.Conv2d(mid, mid, kernel_size=(1, 3), padding=(0, 1), bias=False)
        self.bn3 = nn.BatchNorm2d(mid)
        self.conv4 = nn.Conv2d(mid, ch_out, kernel_size=1, bias=False)
        self.bn4 = nn.BatchNorm2d(ch_out)

        if ch_in != ch_out:
            self.shortcut = nn.Sequential(
                nn.Conv2d(ch_in, ch_out, kernel_size=1, bias=False),
                nn.BatchNorm2d(ch_out),
            )
        else:
            self.shortcut = nn.Identity()
        self.relu = nn.ReLU(inplace=True)

    def forward(self, x):
        identity = self.shortcut(x)
        out = self.relu(self.bn1(self.conv1(x)))
        out = self.relu(self.bn2(self.conv2(out)))
        out = self.relu(self.bn3(self.conv3(out)))
        out = self.bn4(self.conv4(out))
        out = out + identity
        out = self.relu(out)
        return out

class LPRNet(nn.Module):
    def __init__(self, lpr_max_len, phase, class_num, dropout_rate=0.0):
        super(LPRNet, self).__init__()
        self.phase = phase
        self.lpr_max_len = lpr_max_len
        self.class_num = class_num
        # RKNN/ONNX 友好结构：
        # 输入 [N,3,24,94] -> 输出 [N,class_num,18]
        self.backbone = nn.Sequential(
            nn.Conv2d(3, 64, kernel_size=3, stride=1, padding=1, bias=False),
            nn.BatchNorm2d(64),
            nn.ReLU(inplace=True),
            nn.MaxPool2d(kernel_size=2, stride=2),            # 24x94 -> 12x47

            small_basic_block(ch_in=64, ch_out=128),
            small_basic_block(ch_in=128, ch_out=128),
            nn.MaxPool2d(kernel_size=2, stride=2),            # 12x47 -> 6x23

            small_basic_block(ch_in=128, ch_out=256),
            small_basic_block(ch_in=256, ch_out=256),
            small_basic_block(ch_in=256, ch_out=384),
            nn.Dropout(dropout_rate),
        )

        self.classifier = nn.Sequential(
            nn.Conv2d(384, 256, kernel_size=(1, 6), stride=1, bias=False),    # 6x23 -> 6x18
            nn.BatchNorm2d(256),
            nn.ReLU(inplace=True),
            nn.Dropout(dropout_rate),
            nn.Conv2d(256, class_num, kernel_size=(6, 1), stride=1),  # 6x18 -> 1x18
            # logits 输出层不要再接 BN/ReLU，保持 CTC 输入分布稳定
        )
        self._init_weights()

    def _init_weights(self):
        for m in self.modules():
            if isinstance(m, nn.Conv2d):
                nn.init.kaiming_normal_(m.weight, mode='fan_out', nonlinearity='relu')
                if m.bias is not None:
                    nn.init.zeros_(m.bias)
            elif isinstance(m, nn.BatchNorm2d):
                nn.init.ones_(m.weight)
                nn.init.zeros_(m.bias)

    def forward(self, x):
        x = self.backbone(x)
        x = self.classifier(x)
        logits = torch.squeeze(x, dim=2)  # [N, class_num, 18]
        return logits

def build_lprnet(lpr_max_len=8, phase=False, class_num=75, dropout_rate=0.0):

    Net = LPRNet(lpr_max_len, phase, class_num, dropout_rate)

    if phase == "train":
        return Net.train()
    else:
        return Net.eval()
