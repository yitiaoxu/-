# -*- coding: utf-8 -*-
# /usr/bin/env/python3

'''
Pytorch implementation for LPRNet.
Author: aiboy.wei@outlook.com .
'''

from data.load_data import CHARS, CHARS_DICT, LPRDataLoader
from model.LPRNet import build_lprnet
# import torch.backends.cudnn as cudnn
from torch.autograd import Variable
import torch.nn.functional as F
from torch.utils.data import *
from torch.utils.data import DataLoader
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

def str2bool(v):
    if isinstance(v, bool):
        return v
    if v.lower() in ('yes', 'true', 't', '1', 'y'):
        return True
    if v.lower() in ('no', 'false', 'f', '0', 'n'):
        return False
    raise argparse.ArgumentTypeError('Boolean value expected.')

def sparse_tuple_for_ctc(T_length, lengths):
    input_lengths = []
    target_lengths = []

    for ch in lengths:
        input_lengths.append(T_length)
        target_lengths.append(ch)

    return tuple(input_lengths), tuple(target_lengths)

def adjust_learning_rate(optimizer, cur_epoch, base_lr, lr_schedule):
    """
    Sets the learning rate
    """
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
    # === 新增:best 模型替换阈值 ===
    # 只有当 cur_acc - best_acc > min_delta 时,才认为是"真正的提升"并替换 best 权重
    # 推荐 0.001 ~ 0.003,噪声大可以调高
    parser.add_argument('--min_delta', default=0.001, type=float,
                        help='minimum improvement over best_acc required to save new best model')
    # parser.add_argument('--pretrained_model', default='./weights/Final_LPRNet_model.pth', help='pretrained base model')
    parser.add_argument('--pretrained_model', default='', help='pretrained base model')
    args = parser.parse_args()

    return args

def collate_fn(batch):
    imgs = []
    labels = []
    lengths = []
    for _, sample in enumerate(batch):
        img, label, length = sample
        imgs.append(torch.from_numpy(img))
        labels.extend(label)
        lengths.append(length)
    labels = np.asarray(labels).flatten().astype(np.int64)

    return (torch.stack(imgs, 0), torch.from_numpy(labels), lengths)


def decode_logits_to_labels(logits_np, blank_idx):
    # logits_np: [N, C, T]
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
    targets = []
    start = 0
    for length in lengths:
        target = flat_labels[start:start + length].tolist()
        targets.append(target)
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
    """
    统一的 best 判断逻辑（不在训练中落盘）。
    - cur_acc:  本次评估得到的准确率
    - best_acc: 历史最好准确率
    - 返回:    (新的 best_acc, 是否更新了 best)
    只有当 cur_acc - best_acc > min_delta 时,才认为更新了 best。
    """
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


def train():
    args = get_parser()
    args.train_batch_size = FIXED_TRAIN_BATCH_SIZE
    args.test_batch_size = FIXED_TEST_BATCH_SIZE
    args.num_workers = FIXED_NUM_WORKERS

    # T_length 不再写死，改为从每个 batch 的 logits 动态获取时间步
    epoch = 0 + args.resume_epoch
    loss_val = 0

    if not os.path.exists(args.save_folder):
        os.mkdir(args.save_folder)

    lprnet = build_lprnet(
        lpr_max_len=args.lpr_max_len,
        phase="train" if args.phase_train else "test",
        class_num=args.class_num,
        dropout_rate=args.dropout_rate
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

    # load pretrained model
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

    # define optimizer
    # optimizer = optim.SGD(lprnet.parameters(), lr=args.learning_rate,
    #                       momentum=args.momentum, weight_decay=args.weight_decay)
    optimizer = optim.AdamW(
        lprnet.parameters(),
        lr=args.learning_rate,
        weight_decay=1e-4,
        betas=(0.9, 0.999),
    )
    train_img_dirs = os.path.expanduser(args.train_img_dirs)
    test_img_dirs = os.path.expanduser(args.test_img_dirs)
    train_dataset = LPRDataLoader(
        train_img_dirs.split(','),
        args.img_size,
        args.lpr_max_len,
        use_augment=True
    )
    test_dataset = LPRDataLoader(test_img_dirs.split(','), args.img_size, args.lpr_max_len, use_augment=False)
    train_size = len(train_dataset)
    test_size = len(test_dataset)
    print(f"[Info] Dataset size: train={train_size}, test={test_size}")
    if train_size <= 0:
        raise RuntimeError(
            f"Train dataset is empty. Please check --train_img_dirs: {train_img_dirs}"
        )
    if test_size <= 0:
        raise RuntimeError(
            f"Test dataset is empty. Please check --test_img_dirs: {test_img_dirs}"
        )

    epoch_size = max(1, math.ceil(train_size / args.train_batch_size))
    max_iter = args.max_epoch * epoch_size

    ctc_loss = nn.CTCLoss(blank=len(CHARS)-1, reduction='mean') # reduction: 'none' | 'mean' | 'sum'

    if args.resume_epoch > 0:
        start_iter = args.resume_epoch * epoch_size
    else:
        start_iter = 0
    best_acc = 0.0
    best_state_dict = None
    print(f"[Info] min_delta = {args.min_delta}  "
          f"(new acc must exceed best_acc by more than this to be saved as best)")
    print(f"[Info] Fixed params: train_batch_size={args.train_batch_size}, "
          f"test_batch_size={args.test_batch_size}, num_workers={args.num_workers}")

    for iteration in range(start_iter, max_iter):
        if iteration % epoch_size == 0:
            # create batch iterator
            batch_iterator = iter(DataLoader(train_dataset, args.train_batch_size, shuffle=True, num_workers=args.num_workers, collate_fn=collate_fn))
            loss_val = 0
            epoch += 1

        if (iteration + 1) % epoch_size == 0:
            lprnet.eval()
            eval_model = ema.ema if ema is not None else lprnet
            with torch.no_grad():
                cur_acc = Greedy_Decode_Eval(eval_model, test_dataset, args)
            best_acc, is_best = try_update_best(
                cur_acc=cur_acc,
                best_acc=best_acc,
                eval_model=eval_model,
                min_delta=args.min_delta,
                tag=f"[iter {iteration + 1}]",
            )
            if is_best:
                best_state_dict = copy.deepcopy(eval_model.state_dict())
            lprnet.train()

        start_time = time.time()
        # load train data
        images, labels, lengths = next(batch_iterator)
        # labels = np.array([el.numpy() for el in labels]).T
        # print(labels)
        # target_lengths 与标签长度相关；input_lengths 在 forward 后根据实际 T 动态计算
        _, target_lengths = sparse_tuple_for_ctc(0, lengths)
        # update lr
        if args.use_lr_schedule:
            lr = adjust_learning_rate(optimizer, epoch, args.learning_rate, args.lr_schedule)
        else:
            lr = args.learning_rate
            for param_group in optimizer.param_groups:
                param_group['lr'] = lr

        if use_cuda:
            images = Variable(images, requires_grad=False).cuda()
            labels = Variable(labels, requires_grad=False).cuda()
        else:
            images = Variable(images, requires_grad=False)
            labels = Variable(labels, requires_grad=False)

        # forward
        logits = lprnet(images)
        log_probs = logits.permute(2, 0, 1) # for ctc loss: T x N x C
        t_length = int(log_probs.size(0))
        input_lengths = tuple([t_length] * len(lengths))
        # print(labels.shape)
        log_probs = log_probs.log_softmax(2).requires_grad_()
        # log_probs = log_probs.detach().requires_grad_()
        # print(log_probs.shape)
        # backprop
        optimizer.zero_grad()
        loss = ctc_loss(log_probs, labels, input_lengths=input_lengths, target_lengths=target_lengths)
        if loss.item() == np.inf:
            continue
        loss.backward()
        torch.nn.utils.clip_grad_norm_(lprnet.parameters(), max_norm=5.0)
        optimizer.step()
        if ema is not None:
            ema.update(lprnet)
        loss_val += loss.item()
        end_time = time.time()
        if iteration % 20 == 0:
            with torch.no_grad():
                batch_logits = logits.detach().cpu().numpy()  # [N, C, T]
                pred_labels = decode_logits_to_labels(batch_logits, blank_idx=len(CHARS) - 1)
                target_labels = split_targets(labels.detach().cpu(), lengths)
                batch_correct = sum(int(np.asarray(p).shape == np.asarray(t).shape and (np.asarray(p) == np.asarray(t)).all())
                                    for p, t in zip(pred_labels, target_labels))
                batch_acc = batch_correct / max(1, len(target_labels))
                len_match = sum(int(len(p) == len(t)) for p, t in zip(pred_labels, target_labels)) / max(1, len(target_labels))
                char_match = 0
                char_total = 0
                for p, t in zip(pred_labels, target_labels):
                    m = min(len(p), len(t))
                    for k in range(m):
                        char_match += int(p[k] == t[k])
                    char_total += len(t)
                char_acc = char_match / max(1, char_total)
            print('Epoch:' + repr(epoch) + ' || epochiter: ' + repr(iteration % epoch_size) + '/' + repr(epoch_size)
                  + '|| Totel iter ' + repr(iteration) + ' || Loss: %.4f||' % (loss.item()) +
                  'BatchAcc: %.4f ||' % (batch_acc) +
                  'CharAcc: %.4f ||' % (char_acc) +
                  'LenMatch: %.4f ||' % (len_match) +
                  'Batch time: %.4f sec. ||' % (end_time - start_time) + 'LR: %.8f' % (lr))
    # final test
    print("Final test Accuracy:")
    lprnet.eval()
    eval_model = ema.ema if ema is not None else lprnet
    with torch.no_grad():
        final_acc = Greedy_Decode_Eval(eval_model, test_dataset, args)
    best_acc, is_best = try_update_best(
        cur_acc=final_acc,
        best_acc=best_acc,
        eval_model=eval_model,
        min_delta=args.min_delta,
        tag="[final]",
    )
    if is_best:
        best_state_dict = copy.deepcopy(eval_model.state_dict())

    # 仅在全部训练结束后，保存全程最佳模型
    best_path = os.path.join(args.save_folder, 'best_ema.pth' if ema is not None else 'best.pth')
    if best_state_dict is not None:
        torch.save(best_state_dict, best_path)
    else:
        # 未出现超过 min_delta 的提升时，至少保存最终评估模型
        torch.save(eval_model.state_dict(), best_path)
    print(f"[Info] Best model saved at end: {best_path}")
    print(f"[Info] Training done. best_acc = {best_acc:.6f}")

def Greedy_Decode_Eval(Net, datasets, args):
    # TestNet = Net.eval()
    dataset_size = len(datasets)
    if dataset_size <= 0:
        raise RuntimeError("Evaluation dataset is empty, cannot run Greedy_Decode_Eval.")
    epoch_size = max(1, math.ceil(dataset_size / args.test_batch_size))
    batch_iterator = iter(DataLoader(datasets, args.test_batch_size, shuffle=True, num_workers=args.num_workers, collate_fn=collate_fn))

    Tp = 0
    Tn_1 = 0
    Tn_2 = 0
    t1 = time.time()
    for i in range(epoch_size):
        # load train data
        images, labels, lengths = next(batch_iterator)
        start = 0
        targets = []
        for length in lengths:
            label = labels[start:start+length]
            targets.append(label)
            start += length
        targets = [el.numpy() for el in targets]

        if args.cuda and torch.cuda.is_available():
            images = Variable(images.cuda())
        else:
            images = Variable(images)

        # forward
        prebs = Net(images)
        # greedy decode
        prebs = prebs.cpu().detach().numpy()
        preb_labels = list()
        for i in range(prebs.shape[0]):
            preb = prebs[i, :, :]
            preb_label = list()
            for j in range(preb.shape[1]):
                preb_label.append(np.argmax(preb[:, j], axis=0))
            no_repeat_blank_label = list()
            pre_c = preb_label[0]
            if pre_c != len(CHARS) - 1:
                no_repeat_blank_label.append(pre_c)
            for c in preb_label: # dropout repeate label and blank label
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

    Acc = Tp * 1.0 / (Tp + Tn_1 + Tn_2)
    print("[Info] Test Accuracy: {} [{}:{}:{}:{}]".format(Acc, Tp, Tn_1, Tn_2, (Tp+Tn_1+Tn_2)))
    t2 = time.time()
    print("[Info] Test Speed: {}s 1/{}]".format((t2 - t1) / len(datasets), len(datasets)))
    return Acc


if __name__ == "__main__":
    train()

