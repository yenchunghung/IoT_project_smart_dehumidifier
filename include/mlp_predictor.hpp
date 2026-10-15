#pragma once

#include <vector>
#include <cmath>
#include <random>
#include <algorithm>
#include <iostream>

namespace SmartDehum {

class MlpNetwork {
public:
    static constexpr size_t INPUT_DIM = 15;
    static constexpr size_t H1_DIM = 16;
    static constexpr size_t H2_DIM = 8;
    static constexpr size_t OUTPUT_DIM = 1;

    MlpNetwork(double lr = 0.015, double momentum = 0.9)
        : lr_(lr), momentum_(momentum) {
        init_weights();
    }

    // 前向傳播 (Forward Pass)
    double forward(const std::vector<double>& input) {
        last_input_ = input;

        // Layer 1: Input -> H1 (ReLU)
        h1_raw_.assign(H1_DIM, 0.0);
        h1_act_.assign(H1_DIM, 0.0);
        for (size_t j = 0; j < H1_DIM; ++j) {
            double sum = b1_[j];
            for (size_t i = 0; i < INPUT_DIM; ++i) {
                sum += input[i] * w1_[i][j];
            }
            h1_raw_[j] = sum;
            h1_act_[j] = std::max(0.0, sum); // ReLU
        }

        // Layer 2: H1 -> H2 (ReLU)
        h2_raw_.assign(H2_DIM, 0.0);
        h2_act_.assign(H2_DIM, 0.0);
        for (size_t j = 0; j < H2_DIM; ++j) {
            double sum = b2_[j];
            for (size_t i = 0; i < H1_DIM; ++i) {
                sum += h1_act_[i] * w2_[i][j];
            }
            h2_raw_[j] = sum;
            h2_act_[j] = std::max(0.0, sum); // ReLU
        }

        // Layer 3: H2 -> Output (Linear)
        double out = b3_[0];
        for (size_t i = 0; i < H2_DIM; ++i) {
            out += h2_act_[i] * w3_[i][0];
        }

        // 輸出層安全夾鉗：殘差保護限幅 ±2.5 g/m³
        return std::clamp(out, -2.5, 2.5);
    }

    // 反向傳播在線更新 (Backpropagation with Momentum)
    void train_step(const std::vector<double>& input, double target_residual) {
        double pred = forward(input);
        double err = pred - target_residual; // dLoss/dOutput (MSE derivative)

        // 1. Output Layer Gradients
        double d_out = err;
        for (size_t i = 0; i < H2_DIM; ++i) {
            double grad_w3 = d_out * h2_act_[i];
            v_w3_[i][0] = momentum_ * v_w3_[i][0] + lr_ * grad_w3;
            w3_[i][0] -= v_w3_[i][0];
        }
        v_b3_[0] = momentum_ * v_b3_[0] + lr_ * d_out;
        b3_[0] -= v_b3_[0];

        // 2. Hidden Layer 2 Gradients
        std::vector<double> d_h2(H2_DIM, 0.0);
        for (size_t i = 0; i < H2_DIM; ++i) {
            double d_relu = (h2_raw_[i] > 0.0) ? 1.0 : 0.0;
            d_h2[i] = (d_out * w3_[i][0]) * d_relu;
        }

        for (size_t i = 0; i < H1_DIM; ++i) {
            for (size_t j = 0; j < H2_DIM; ++j) {
                double grad_w2 = d_h2[j] * h1_act_[i];
                v_w2_[i][j] = momentum_ * v_w2_[i][j] + lr_ * grad_w2;
                w2_[i][j] -= v_w2_[i][j];
            }
        }
        for (size_t j = 0; j < H2_DIM; ++j) {
            v_b2_[j] = momentum_ * v_b2_[j] + lr_ * d_h2[j];
            b2_[j] -= v_b2_[j];
        }

        // 3. Hidden Layer 1 Gradients
        std::vector<double> d_h1(H1_DIM, 0.0);
        for (size_t i = 0; i < H1_DIM; ++i) {
            double sum = 0.0;
            for (size_t j = 0; j < H2_DIM; ++j) {
                sum += d_h2[j] * w2_[i][j];
            }
            double d_relu = (h1_raw_[i] > 0.0) ? 1.0 : 0.0;
            d_h1[i] = sum * d_relu;
        }

        for (size_t i = 0; i < INPUT_DIM; ++i) {
            for (size_t j = 0; j < H1_DIM; ++j) {
                double grad_w1 = d_h1[j] * input[i];
                v_w1_[i][j] = momentum_ * v_w1_[i][j] + lr_ * grad_w1;
                w1_[i][j] -= v_w1_[i][j];
            }
        }
        for (size_t j = 0; j < H1_DIM; ++j) {
            v_b1_[j] = momentum_ * v_b1_[j] + lr_ * d_h1[j];
            b1_[j] -= v_b1_[j];
        }
    }

private:
    double lr_;
    double momentum_;

    // 權重與偏置 (Weights & Biases)
    std::vector<std::vector<double>> w1_, w2_, w3_;
    std::vector<double> b1_, b2_, b3_;

    // 動量快取 (Momentum Velocity)
    std::vector<std::vector<double>> v_w1_, v_w2_, v_w3_;
    std::vector<double> v_b1_, v_b2_, v_b3_;

    // 前向傳播暫存 (Forward Cache)
    std::vector<double> last_input_;
    std::vector<double> h1_raw_, h1_act_;
    std::vector<double> h2_raw_, h2_act_;

    void init_weights() {
        std::mt19937 gen(42);
        std::normal_distribution<double> dist_h1(0.0, std::sqrt(2.0 / INPUT_DIM));
        std::normal_distribution<double> dist_h2(0.0, std::sqrt(2.0 / H1_DIM));
        std::normal_distribution<double> dist_out(0.0, 0.01); // 輸出層給極小值

        w1_.assign(INPUT_DIM, std::vector<double>(H1_DIM));
        v_w1_.assign(INPUT_DIM, std::vector<double>(H1_DIM, 0.0));
        b1_.assign(H1_DIM, 0.0);
        v_b1_.assign(H1_DIM, 0.0);
        for (size_t i = 0; i < INPUT_DIM; ++i) {
            for (size_t j = 0; j < H1_DIM; ++j) w1_[i][j] = dist_h1(gen);
        }

        w2_.assign(H1_DIM, std::vector<double>(H2_DIM));
        v_w2_.assign(H1_DIM, std::vector<double>(H2_DIM, 0.0));
        b2_.assign(H2_DIM, 0.0);
        v_b2_.assign(H2_DIM, 0.0);
        for (size_t i = 0; i < H1_DIM; ++i) {
            for (size_t j = 0; j < H2_DIM; ++j) w2_[i][j] = dist_h2(gen);
        }

        w3_.assign(H2_DIM, std::vector<double>(OUTPUT_DIM));
        v_w3_.assign(H2_DIM, std::vector<double>(OUTPUT_DIM, 0.0));
        b3_.assign(OUTPUT_DIM, 0.0);
        v_b3_.assign(OUTPUT_DIM, 0.0);
        for (size_t i = 0; i < H2_DIM; ++i) {
            for (size_t j = 0; j < OUTPUT_DIM; ++j) w3_[i][j] = dist_out(gen);
        }
    }
};

} // namespace SmartDehum
