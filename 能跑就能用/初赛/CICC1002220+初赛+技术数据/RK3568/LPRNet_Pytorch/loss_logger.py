import os
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


class LossLogger:
    """
    训练曲线日志：
      - CSV 增量追加，断电不丢历史
      - PNG 定期重写，远程拷贝即可看
      - 四联图：loss(对数) / batch_acc / char_acc / lr
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

        # CTC Loss（对数轴：CTC 跨度大，对数更直观）
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


if __name__ == "__main__":
    # 模拟一段过拟合测试的训练曲线
    np.random.seed(0)
    logger = LossLogger('/tmp/test_log', plot_every=50)
    for i in range(500):
        # 模拟 CTC loss 从 ~4.3 一路下降到接近 0（典型 overfit 测试形态）
        base = 4.3 * np.exp(-i / 80) + 0.02
        loss = max(1e-3, base + 0.15 * np.random.randn() * (1 + np.exp(-i/60)))
        bacc = min(1.0, max(0.0, 1.0 - np.exp(-i / 100) * (1.0 + 0.05 * np.random.randn())))
        cacc = min(1.0, max(0.0, 1.0 - np.exp(-i / 70) * (1.0 + 0.03 * np.random.randn())))
        lr = 1e-3
        logger.log(i, i // 50, loss, bacc, cacc, lr)
    logger.finalize()
    print("OK")

