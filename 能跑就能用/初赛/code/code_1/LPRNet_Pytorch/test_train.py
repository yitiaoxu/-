# -*- coding: utf-8 -*-
# /usr/bin/env/python3

'''
Pytorch implementation for LPRNet.
Author: aiboy.wei@outlook.com .

本版改动：
  1. 新增 --overfit_test 模式：用极小子集反复训练以排查"模型容量 / 优化"问题。
     该模式会强制 dropout=0、关 EMA、关 LR schedule、关数据增强、跳过 eval。
  2. 新增训练曲线记录（LossLogger）：CSV 增量日志 + PNG 定期保存
     （loss / batch_acc / char_acc / lr）。
'''

from data.load_data import CHARS, CHARS_DICT, LPRDataLoader
from model.LPRNet import build_lprnet

# matplotlib 必须在 import pyplot 之前设置 Agg 后端，避免远程/无显示器环境出错
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

from torch.autograd import Variable
import torch.nn.functional as F
from torch.utils.data import DataLoader, Subset
from torch import optim
import torch.nn as nn
import numpy as np
import argparse
import torch
import time
import os
import copy
import math

# 固定训练相关参数（不通过命令行改动）
FIXED_TRAIN_BATCH_SIZE = 128
FIXED_TEST_BATCH_SIZE = 120
FIXED_NUM_WORKERS = 8


# ---------------------------------------------------------------------------
# 训练曲线记录器
# ---------------------------------------------------------------------------
class LossLogger:
    """
    训练曲线日志：
      - CSV 增量追加，断电不丢历史；
      - PNG 定期重写，远程 scp 拷贝即可看；
      - 四联图：CTC Loss(对数) / Batch Acc / Char Acc / LR。
    """
    def __init__(self, save_dir, plot_every=200, smooth_window=None,
                 png_name='train_curve.png', csv_name='train_log.csv'):
        os.makedirs(save_dir, exist_ok=True)
        self.save_dir = save_dir
        self.plot_every = max(1, plot_every)
        self.smooth_window = smooth_window
        self.iters, self.losses = [], []
        self.batch_accs, self.char_accs, self.lrs = [], [], []
        self.csv_path = os.path.join(save_dir, csv_name)
        self.png_path = os.path.join(save_dir, png_name)
        with open(self.csv_path, 'w') as f:
            f.write('iter,epoch,loss,batch_acc,char_acc,lr\n')

    def log(self, it, epoch, loss, batch_acc, char_acc, lr):
        self.iters.append(int(it))
        self.losses.append(float(loss))
        self.batch_accs.append(float(batch_acc))
        self.char_accs.append(float(char_acc))
        self.lrs.append(float(lr))
        with open(self.csv_path, 'a') as f:
            f.write(f'{it},{epoch},{loss:.6f},{batch_acc:.6f},'
                    f'{char_acc:.6f},{lr:.8f}\n')
        if len(self.iters) % self.plot_every == 0:
            self.save_plot()

    def _smooth(self, arr, win):
        if len(arr) < win:
            return None
        return np.convolve(arr, np.ones(win) / win, mode='valid')

    def save_plot(self):
        if len(self.iters) < 2:
            return
        win = self.smooth_window or max(5, len(self.losses) // 50)
        fig, axes = plt.subplots(2, 2, figsize=(13, 8))

        # CTC Loss 对数轴
        ax = axes[0, 0]
        ax.plot(self.iters, self.losses, color='tab:red', alpha=0.35, label='raw')
        sm = self._smooth(self.losses, win)
        if sm is not None:
            ax.plot(self.iters[win - 1:], sm, color='tab:red',
                    linewidth=2, label=f'mean({win})')
        ax.set_title('CTC Loss')
        ax.set_xlabel('iteration'); ax.set_ylabel('loss')
        ax.set_yscale('log'); ax.grid(True, alpha=0.3); ax.legend()

        # 整车牌准确率
        ax = axes[0, 1]
        ax.plot(self.iters, self.batch_accs, color='tab:blue', alpha=0.35, label='raw')
        sm = self._smooth(self.batch_accs, win)
        if sm is not None:
            ax.plot(self.iters[win - 1:], sm, color='tab:blue',
                    linewidth=2, label=f'mean({win})')
        ax.set_title('Batch Accuracy (whole plate)')
        ax.set_xlabel('iteration'); ax.set_ylabel('acc')
        ax.set_ylim(-0.02, 1.02); ax.grid(True, alpha=0.3); ax.legend()

        # 字符级准确率
        ax = axes[1, 0]
        ax.plot(self.iters, self.char_accs, color='tab:green', alpha=0.35, label='raw')
        sm = self._smooth(self.char_accs, win)
        if sm is not None:
            ax.plot(self.iters[win - 1:], sm, color='tab:green',
                    linewidth=2, label=f'mean({win})')
        ax.set_title('Char Accuracy')
        ax.set_xlabel('iteration'); ax.set_ylabel('acc')
        ax.set_ylim(-0.02, 1.02); ax.grid(True, alpha=0.3); ax.legend()

        # 学习率
        ax = axes[1, 1]
        ax.plot(self.iters, self.lrs, color='tab:purple')
        ax.set_title('Learning Rate')
        ax.set_xlabel('iteration'); ax.set_ylabel('lr')
        ax.set_yscale('log'); ax.grid(True, alpha=0.3)

        plt.tight_layout()
        plt.savefig(self.png_path, dpi=100, bbox_inches='tight')
        plt.close(fig)

    def finalize(self):
        self.save_plot()
        print(f"[Info] Loss curve saved to: {self.png_path}")
        print(f"[Info] CSV log saved to:    {self.csv_path}")


# ---------------------------------------------------------------------------
# 工具函数（保持原版逻辑）
# ---------------------------------------------------------------------------
def str2bool(v):
    if isinstance(v, bool):
        return v
    if v.lower() in ('yes', 'true', 't', '1', 'y'):
        return True
    if v.lower() in ('no', 'false', 'f', '0', 'n'):
        return False
    raise argparse.ArgumentTypeError('Boolean value expected.')


def sparse_tuple_for_ctc(T_length, lengths):
    input_lengths, target_lengths = [], []
    for ch in lengths:
        input_lengths.append(T_length)
        target_lengths.append(ch)
    return tuple(input_lengths), tuple(target_lengths)


def adjust_learning_rate(optimizer, cur_epoch, base_lr, lr_schedule):
    """Sets the learning rate"""
    decay_steps = sum(cur_epoch >= e for e in lr_schedule)
    lr = base_lr * (0.1 ** decay_steps)
    for param_group in optimizer.param_groups:
        param_group['lr'] = lr
    return lr


def get_parser():
    parser = argparse.ArgumentParser(description='parameters to train net')
    parser.add_argument('--max_epoch', default=15, type=int, help='epoch to train the network')
    parser.add_argument('--img_size', default=[94, 24], nargs=2, type=int, help='the image size [w h]')
    parser.add_argument('--train_img_dirs', default="~/workspace/trainMixLPR", help='the train images path')
    parser.add_argument('--test_img_dirs', default="~/workspace/testMixLPR", help='the test images path')
    parser.add_argument('--dropout_rate', default=0.0, type=float, help='dropout rate.')
    parser.add_argument('--class_num', default=75, type=int, help='number of output classes.')
    parser.add_argument('--learning_rate', default=1e-4, type=float, help='base value of learning rate.')
    parser.add_argument('--use_lr_schedule', default=True, type=str2bool, help='use lr schedule or fixed lr')
    parser.add_argument('--lpr_max_len', default=8, type=int, help='license plate number max length.')
    parser.add_argument('--train_batch_size', default=128, type=int, help='training batch size.')
    parser.add_argument('--test_batch_size', default=120, type=int, help='testing batch size.')
    parser.add_argument('--phase_train', default=True, type=str2bool, help='train or test phase flag.')
    parser.add_argument('--num_workers', default=8, type=int, help='Number of workers used in dataloading')
    parser.add_argument('--cuda', default=True, type=str2bool, help='Use cuda to train model')
    parser.add_argument('--resume_epoch', default=0, type=int, help='resume iter for retraining')
    parser.add_argument('--momentum', default=0.9, type=float, help='momentum')
    parser.add_argument('--weight_decay', default=2e-5, type=float, help='Weight decay for SGD')
    parser.add_argument('--lr_schedule', default=[4, 8, 12, 14, 16], nargs='+', type=int, help='schedule for learning rate')
    parser.add_argument('--save_folder', default='./weights/', help='Location to save checkpoint models')
    parser.add_argument('--use_ema', default=True, type=str2bool, help='use EMA for eval and best save')
    parser.add_argument('--ema_decay', default=0.9995, type=float, help='EMA decay factor')
    parser.add_argument('--min_delta', default=0.001, type=float,
                        help='minimum improvement over best_acc required to save new best model')
    parser.add_argument('--pretrained_model', default='', help='pretrained base model')

    # ===== 新增: 过拟合测试 =====
    parser.add_argument('--overfit_test', default=False, type=str2bool,
                        help='过拟合测试模式: 用极小子集反复训练, 验证模型容量与优化路径')
    parser.add_argument('--overfit_samples', default=64, type=int,
                        help='过拟合测试使用的样本数')
    parser.add_argument('--overfit_iters', default=2000, type=int,
                        help='过拟合测试的总迭代步数')

    # ===== 新增: 日志与曲线 =====
    parser.add_argument('--log_dir', default='./logs/', help='训练曲线与 CSV 日志输出目录')
    parser.add_argument('--log_every', default=20, type=int, help='打印日志/记录曲线点的间隔(iter)')
    parser.add_argument('--plot_every', default=200, type=int, help='保存曲线 PNG 的间隔(iter)')

    args = parser.parse_args()
    return args


def collate_fn(batch):
    imgs, labels, lengths = [], [], []
    for _, sample in enumerate(batch):
        img, label, length = sample
        imgs.append(torch.from_numpy(img))
        labels.extend(label)
        lengths.append(length)
    labels = np.asarray(labels).flatten().astype(np.int64)
    return (torch.stack(imgs, 0), torch.from_numpy(labels), lengths)


def decode_logits_to_labels(logits_np, blank_idx):
    pred_labels = []
    for i in range(logits_np.shape[0]):
        preb = logits_np[i, :, :]
        preb_label = [int(np.argmax(preb[:, j], axis=0)) for j in range(preb.shape[1])]
        no_repeat_blank_label = []
        pre_c = preb_label[0]
        if pre_c != blank_idx:
            no_repeat_blank_label.append(pre_c)
        for c in preb_label:
            if (pre_c == c) or (c == blank_idx):
                if c == blank_idx:
                    pre_c = c
                continue
            no_repeat_blank_label.append(c)
            pre_c = c
        pred_labels.append(no_repeat_blank_label)
    return pred_labels


def split_targets(flat_labels, lengths):
    targets, start = [], 0
    for length in lengths:
        targets.append(flat_labels[start:start + length].tolist())
        start += length
    return targets


class ModelEMA:
    def __init__(self, model, decay=0.9995):
        self.ema = copy.deepcopy(model).eval()
        for p in self.ema.parameters():
            p.requires_grad_(False)
        self.decay = decay

    @torch.no_grad()
    def update(self, model):
        src_state = model.state_dict()
        for k, v in self.ema.state_dict().items():
            src_v = src_state[k].detach()
            if v.dtype.is_floating_point:
                v.mul_(self.decay).add_(src_v, alpha=1.0 - self.decay)
            else:
                v.copy_(src_v)


def try_update_best(cur_acc, best_acc, eval_model, min_delta, tag=""):
    improvement = cur_acc - best_acc
    prefix = f"[Info]{(' ' + tag) if tag else ''}"
    if improvement > min_delta:
        new_best = cur_acc
        print(f"{prefix} New best updated in memory, "
              f"acc={new_best:.6f} (improvement +{improvement:.6f} > min_delta {min_delta})")
        return new_best, True
    elif cur_acc > best_acc:
        print(f"{prefix} acc={cur_acc:.6f} > best={best_acc:.6f}, "
              f"but improvement +{improvement:.6f} <= min_delta {min_delta}, skip saving.")
    else:
        print(f"{prefix} acc={cur_acc:.6f} <= best={best_acc:.6f}, skip saving.")
    return best_acc, False


# ---------------------------------------------------------------------------
# 数据集准备：根据是否 overfit_test 决定增强与采样
# ---------------------------------------------------------------------------
def prepare_datasets(args):
    train_img_dirs = os.path.expanduser(args.train_img_dirs)
    test_img_dirs = os.path.expanduser(args.test_img_dirs)

    # overfit_test 模式: 关增强; 正常模式: 训练集开增强、测试集关
    use_aug_train = (not args.overfit_test)

    train_full = LPRDataLoader(
        train_img_dirs.split(','),
        args.img_size,
        args.lpr_max_len,
        use_augment=use_aug_train,
    )

    if args.overfit_test:
        n = min(args.overfit_samples, len(train_full))
        if n <= 0:
            raise RuntimeError("Train dataset is empty for overfit test.")
        train_dataset = Subset(train_full, list(range(n)))
        # 测试集就用训练子集本身（overfit 测试不需要 generalization）
        test_dataset = train_dataset
        # batch 不能超过样本数
        args.train_batch_size = min(args.train_batch_size, n)
        args.test_batch_size = min(args.test_batch_size, n)
        print(f"[OverfitTest] Using {n} training samples, "
              f"train_batch_size={args.train_batch_size}, "
              f"test_batch_size={args.test_batch_size}")
    else:
        train_dataset = train_full
        test_dataset = LPRDataLoader(
            test_img_dirs.split(','),
            args.img_size,
            args.lpr_max_len,
            use_augment=False,
        )

    return train_dataset, test_dataset


# ---------------------------------------------------------------------------
# 训练主流程
# ---------------------------------------------------------------------------
def train():
    args = get_parser()
    script_dir = os.path.dirname(os.path.abspath(__file__))

    # 统一输出目录为脚本所在目录下的子目录，避免受启动 cwd 影响
    if not os.path.isabs(args.log_dir):
        args.log_dir = os.path.join(script_dir, args.log_dir)

    # ----- 过拟合测试模式: 覆盖一组关键参数 -----
    if args.overfit_test:
        print("=" * 70)
        print("[OverfitTest] 开启过拟合测试模式")
        print("[OverfitTest]   - dropout_rate     -> 0.0 (强制)")
        print("[OverfitTest]   - use_ema          -> False (强制)")
        print("[OverfitTest]   - use_lr_schedule  -> False (强制)")
        print("[OverfitTest]   - use_augment      -> False (强制)")
        print("[OverfitTest]   - eval / best save -> 跳过")
        print(f"[OverfitTest]   - overfit_samples = {args.overfit_samples}")
        print(f"[OverfitTest]   - overfit_iters   = {args.overfit_iters}")
        print(f"[OverfitTest]   - learning_rate   = {args.learning_rate}")
        print("[OverfitTest] 期望: loss 最终能逼近 0, batch_acc -> 1.0")
        print("[OverfitTest]   若 loss 卡住不动 -> 优化或容量问题")
        print("[OverfitTest]   若 loss 顺利降到 0 -> 模型本身没问题, "
              "转去查正则/增强/数据规模")
        print("=" * 70)
        args.dropout_rate = 0.0
        args.use_ema = False
        args.use_lr_schedule = False
        args.num_workers = min(FIXED_NUM_WORKERS, 2)  # 小数据集不需要多 worker
    else:
        args.train_batch_size = FIXED_TRAIN_BATCH_SIZE
        args.test_batch_size = FIXED_TEST_BATCH_SIZE
        args.num_workers = FIXED_NUM_WORKERS

    epoch = 0 + args.resume_epoch
    loss_val = 0

    if not os.path.exists(args.save_folder):
        os.makedirs(args.save_folder, exist_ok=True)

    # ----- 模型 -----
    lprnet = build_lprnet(
        lpr_max_len=args.lpr_max_len,
        phase="train" if args.phase_train else "test",
        class_num=args.class_num,
        dropout_rate=args.dropout_rate,
    )
    if args.class_num != len(CHARS):
        print(f"[Warn] class_num({args.class_num}) != len(CHARS)({len(CHARS)}). "
              "Please ensure label dictionary and model output classes are aligned.")
    use_cuda = args.cuda and torch.cuda.is_available()
    device = torch.device("cuda:0" if use_cuda else "cpu")
    print(f"[Info] Training device: {device}")
    if args.cuda and not torch.cuda.is_available():
        print("[Warn] --cuda=true but CUDA is unavailable, fallback to CPU.")
    lprnet.to(device)
    print("Successful to build network!")
    lprnet.train()
    ema = ModelEMA(lprnet, decay=args.ema_decay) if args.use_ema else None

    # ----- 预训练权重 -----
    if args.pretrained_model:
        lprnet.load_state_dict(torch.load(args.pretrained_model))
        print("load pretrained model successful!")
    else:
        def weights_init(m):
            if isinstance(m, nn.Conv2d):
                nn.init.kaiming_normal_(m.weight, mode='fan_out', nonlinearity='relu')
                if m.bias is not None:
                    nn.init.constant_(m.bias, 0.0)
            elif isinstance(m, nn.BatchNorm2d):
                nn.init.constant_(m.weight, 1.0)
                nn.init.constant_(m.bias, 0.0)
        lprnet.backbone.apply(weights_init)
        lprnet.classifier.apply(weights_init)
        print("initial net weights successful!")

    # ----- 优化器 -----
    optimizer = optim.AdamW(
        lprnet.parameters(),
        lr=args.learning_rate,
        weight_decay=1e-4,
        betas=(0.9, 0.999),
    )

    # ----- 数据集 -----
    train_dataset, test_dataset = prepare_datasets(args)
    train_size = len(train_dataset)
    test_size = len(test_dataset)
    print(f"[Info] Dataset size: train={train_size}, test={test_size}")
    if train_size <= 0:
        raise RuntimeError(f"Train dataset is empty: {args.train_img_dirs}")
    if test_size <= 0:
        raise RuntimeError(f"Test dataset is empty: {args.test_img_dirs}")

    epoch_size = max(1, math.ceil(train_size / args.train_batch_size))

    # ----- 训练步数: overfit 模式用 overfit_iters 覆盖 -----
    if args.overfit_test:
        max_iter = args.overfit_iters
    else:
        max_iter = args.max_epoch * epoch_size

    ctc_loss = nn.CTCLoss(blank=len(CHARS) - 1, reduction='mean', zero_infinity=True)

    start_iter = args.resume_epoch * epoch_size if args.resume_epoch > 0 else 0
    best_acc = 0.0
    best_state_dict = None

    # ----- 日志 -----
    log_subdir = 'overfit_test' if args.overfit_test else 'train'
    logger = LossLogger(
        save_dir=os.path.join(args.log_dir, log_subdir),
        plot_every=args.plot_every,
    )

    print(f"[Info] min_delta = {args.min_delta}")
    print(f"[Info] batch_size: train={args.train_batch_size}, "
          f"test={args.test_batch_size}, num_workers={args.num_workers}")
    print(f"[Info] epoch_size = {epoch_size}, max_iter = {max_iter}")
    print(f"[Info] log dir: {logger.save_dir}")

    # ----- 训练循环 -----
    batch_iterator = None
    for iteration in range(start_iter, max_iter):
        if iteration % epoch_size == 0:
            batch_iterator = iter(DataLoader(
                train_dataset, args.train_batch_size, shuffle=True,
                num_workers=args.num_workers, collate_fn=collate_fn,
            ))
            loss_val = 0
            epoch += 1

        # epoch 末尾 eval（overfit 模式跳过, 省时间）
        if (not args.overfit_test) and (iteration + 1) % epoch_size == 0:
            lprnet.eval()
            eval_model = ema.ema if ema is not None else lprnet
            with torch.no_grad():
                cur_acc = Greedy_Decode_Eval(eval_model, test_dataset, args)
            best_acc, is_best = try_update_best(
                cur_acc=cur_acc, best_acc=best_acc,
                eval_model=eval_model, min_delta=args.min_delta,
                tag=f"[iter {iteration + 1}]",
            )
            if is_best:
                best_state_dict = copy.deepcopy(eval_model.state_dict())
            lprnet.train()

        start_time = time.time()
        try:
            images, labels, lengths = next(batch_iterator)
        except StopIteration:
            # overfit 模式下 epoch_size 很小, 可能在中途用完, 重启迭代器
            batch_iterator = iter(DataLoader(
                train_dataset, args.train_batch_size, shuffle=True,
                num_workers=args.num_workers, collate_fn=collate_fn,
            ))
            images, labels, lengths = next(batch_iterator)

        _, target_lengths = sparse_tuple_for_ctc(0, lengths)

        # 学习率
        if args.use_lr_schedule:
            lr = adjust_learning_rate(optimizer, epoch, args.learning_rate, args.lr_schedule)
        else:
            lr = args.learning_rate
            for param_group in optimizer.param_groups:
                param_group['lr'] = lr

        if use_cuda:
            images = images.cuda(non_blocking=True)
            labels = labels.cuda(non_blocking=True)

        logits = lprnet(images)
        log_probs = logits.permute(2, 0, 1)  # T x N x C
        t_length = int(log_probs.size(0))
        input_lengths = tuple([t_length] * len(lengths))
        log_probs = log_probs.log_softmax(2).requires_grad_()

        optimizer.zero_grad()
        loss = ctc_loss(log_probs, labels,
                        input_lengths=input_lengths,
                        target_lengths=target_lengths)
        if loss.item() == np.inf or torch.isnan(loss):
            print(f"[Warn] iter {iteration}: loss is inf/nan, skip this step.")
            continue
        loss.backward()
        torch.nn.utils.clip_grad_norm_(lprnet.parameters(), max_norm=5.0)
        optimizer.step()
        if ema is not None:
            ema.update(lprnet)
        loss_val += loss.item()
        end_time = time.time()

        # ----- 周期性日志记录与打印 -----
        if iteration % args.log_every == 0:
            with torch.no_grad():
                batch_logits = logits.detach().cpu().numpy()
                pred_labels = decode_logits_to_labels(batch_logits, blank_idx=len(CHARS) - 1)
                target_labels = split_targets(labels.detach().cpu(), lengths)
                batch_correct = sum(
                    int(np.asarray(p).shape == np.asarray(t).shape and
                        (np.asarray(p) == np.asarray(t)).all())
                    for p, t in zip(pred_labels, target_labels)
                )
                batch_acc = batch_correct / max(1, len(target_labels))
                len_match = sum(int(len(p) == len(t))
                                for p, t in zip(pred_labels, target_labels)) / max(1, len(target_labels))
                char_match, char_total = 0, 0
                for p, t in zip(pred_labels, target_labels):
                    m = min(len(p), len(t))
                    for k in range(m):
                        char_match += int(p[k] == t[k])
                    char_total += len(t)
                char_acc = char_match / max(1, char_total)

            print('Epoch:' + repr(epoch)
                  + ' || epochiter: ' + repr(iteration % epoch_size) + '/' + repr(epoch_size)
                  + ' || Total iter ' + repr(iteration)
                  + ' || Loss: %.4f ||' % (loss.item())
                  + ' BatchAcc: %.4f ||' % (batch_acc)
                  + ' CharAcc: %.4f ||' % (char_acc)
                  + ' LenMatch: %.4f ||' % (len_match)
                  + ' Batch time: %.4f sec. ||' % (end_time - start_time)
                  + ' LR: %.8f' % (lr))

            # 记录到曲线日志
            logger.log(
                it=iteration, epoch=epoch, loss=loss.item(),
                batch_acc=batch_acc, char_acc=char_acc, lr=lr,
            )

    # ----- 最终评估 / 保存 -----
    if not args.overfit_test:
        print("Final test Accuracy:")
        lprnet.eval()
        eval_model = ema.ema if ema is not None else lprnet
        with torch.no_grad():
            final_acc = Greedy_Decode_Eval(eval_model, test_dataset, args)
        best_acc, is_best = try_update_best(
            cur_acc=final_acc, best_acc=best_acc,
            eval_model=eval_model, min_delta=args.min_delta,
            tag="[final]",
        )
        if is_best:
            best_state_dict = copy.deepcopy(eval_model.state_dict())

        best_path = os.path.join(args.save_folder,
                                 'best_ema.pth' if ema is not None else 'best.pth')
        if best_state_dict is not None:
            torch.save(best_state_dict, best_path)
        else:
            torch.save(eval_model.state_dict(), best_path)
        print(f"[Info] Best model saved at end: {best_path}")
        print(f"[Info] Training done. best_acc = {best_acc:.6f}")
    else:
        # overfit 测试结束后, 给个明确的诊断结论
        print("=" * 70)
        print("[OverfitTest] 最终诊断")
        last_n = min(20, len(logger.losses))
        if last_n > 0:
            recent_loss = float(np.mean(logger.losses[-last_n:]))
            recent_bacc = float(np.mean(logger.batch_accs[-last_n:]))
            recent_cacc = float(np.mean(logger.char_accs[-last_n:]))
            print(f"[OverfitTest] 最近 {last_n} 个记录点的均值:")
            print(f"[OverfitTest]   loss      = {recent_loss:.6f}")
            print(f"[OverfitTest]   batch_acc = {recent_bacc:.6f}")
            print(f"[OverfitTest]   char_acc  = {recent_cacc:.6f}")
            if recent_loss < 0.05 and recent_bacc > 0.95:
                print("[OverfitTest] => 通过: 模型容量与优化器都没问题。")
                print("[OverfitTest]    若实际训练仍欠拟合, 应排查: "
                      "数据增强是否过强 / 正则是否过强 / 数据集本身是否有歧义标注。")
            elif recent_loss < 1.0:
                print("[OverfitTest] => 部分通过: loss 在下降但未收敛到 0。")
                print("[OverfitTest]    建议: 增加 overfit_iters 再观察; "
                      "或调大 learning_rate 试试。")
            else:
                print("[OverfitTest] => 未通过: loss 没有有效下降。")
                print("[OverfitTest]    可能原因 (按可能性排序):")
                print("[OverfitTest]      1) 学习率不合适 (太大/太小)")
                print("[OverfitTest]      2) 标签字典与 class_num 不一致 / blank 索引错误")
                print("[OverfitTest]      3) CTC T < 2L-1 (输出时间步太短, 装不下标签)")
                print("[OverfitTest]      4) 模型容量确实不足 (排在最后, 极少见)")
        print("=" * 70)

    logger.finalize()


def Greedy_Decode_Eval(Net, datasets, args):
    dataset_size = len(datasets)
    if dataset_size <= 0:
        raise RuntimeError("Evaluation dataset is empty, cannot run Greedy_Decode_Eval.")
    epoch_size = max(1, math.ceil(dataset_size / args.test_batch_size))
    batch_iterator = iter(DataLoader(
        datasets, args.test_batch_size, shuffle=True,
        num_workers=args.num_workers, collate_fn=collate_fn,
    ))

    Tp, Tn_1, Tn_2 = 0, 0, 0
    t1 = time.time()
    for i in range(epoch_size):
        images, labels, lengths = next(batch_iterator)
        start, targets = 0, []
        for length in lengths:
            label = labels[start:start + length]
            targets.append(label)
            start += length
        targets = [el.numpy() for el in targets]

        if args.cuda and torch.cuda.is_available():
            images = images.cuda(non_blocking=True)

        prebs = Net(images).cpu().detach().numpy()
        preb_labels = []
        for i in range(prebs.shape[0]):
            preb = prebs[i, :, :]
            preb_label = [int(np.argmax(preb[:, j], axis=0)) for j in range(preb.shape[1])]
            no_repeat_blank_label = []
            pre_c = preb_label[0]
            if pre_c != len(CHARS) - 1:
                no_repeat_blank_label.append(pre_c)
            for c in preb_label:
                if (pre_c == c) or (c == len(CHARS) - 1):
                    if c == len(CHARS) - 1:
                        pre_c = c
                    continue
                no_repeat_blank_label.append(c)
                pre_c = c
            preb_labels.append(no_repeat_blank_label)
        for i, label in enumerate(preb_labels):
            target_i = targets[i]
            if len(label) != len(target_i):
                Tn_1 += 1
                continue
            if (np.asarray(target_i) == np.asarray(label)).all():
                Tp += 1
            else:
                Tn_2 += 1

    Acc = Tp * 1.0 / max(1, (Tp + Tn_1 + Tn_2))
    print("[Info] Test Accuracy: {} [{}:{}:{}:{}]".format(Acc, Tp, Tn_1, Tn_2, (Tp + Tn_1 + Tn_2)))
    t2 = time.time()
    print("[Info] Test Speed: {}s 1/{}]".format((t2 - t1) / max(1, len(datasets)), len(datasets)))
    return Acc


if __name__ == "__main__":
    train()

