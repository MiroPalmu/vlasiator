#include "ionosphere_gpu_solver.hpp"
#include "tools.hpp"
#include <cassert>
#include <numeric>
#include <cmath>
#include <vector>
#include <array>

using test_type = double;

auto main() -> int {
    
    const auto v = std::vector<test_type>{
        0, 
        -4464, 
        -4928, 
        -4592, 
        -4956, 
        -900, 
        -4684, 
        -5048, 
        -5212, 
        -5476, 
        -900, 
        -5804, 
        -4868, 
        -5432, 
        -5296, 
        -600, 
        -4724, 
        -4688, 
        -5452, 
        -4916, 
        -800, 
        -5544, 
        -5308, 
        -3772, 
        -4936, 
        -350, 
        -4964, 
        -6028, 
        -4892, 
        -3956, 
        -700, 
        -5084, 
        -4948, 
        -5612, 
        -4876, 
        -800, 
        -5304, 
        -4968, 
        -5032, 
        -4796, 
        -800, 
        -4524, 
        -5488, 
        -4152, 
        -5016, 
        -500, 
        -4244, 
        -5308, 
        -4672, 
        -4836, 
        -50, 
        -5164, 
        -5328, 
        -4692, 
        -5756, 
        -1100, 
        -4984, 
        -5848, 
        -4512, 
        -5476, 
        -800, 
        -5204, 
        -4968, 
        -5032, 
        -4696, 
        -800, 
        -5124, 
        -4388, 
        -5052, 
        -4916, 
        -900, 
        -6044, 
        -5108, 
        -3972, 
        -5036, 
        -350, 
        -5064, 
        -6228, 
        -4692, 
        -4456, 
        -800, 
        -5084, 
        -4548, 
        -5312, 
        -5276, 
        -1000, 
        -4704, 
        -4568, 
        -5132, 
        -4196, 
        -700, 
        -4524, 
        -4788, 
        -4952, 
        -5316, 
        -700, 
        -5044, 
        -5408, 
        -5072, 
        -5536
    };
    const auto norm_correct = std::accumulate(v.begin(), v.end(), 0.0, [](const auto s, const auto x) { return s + x * x; });

    const auto norm = ionogpu::vectorNormSquared<test_type>(v);
    
    // This is jank but works 
    const auto [absolute_error, relative_error] = ionogpu::testing::calculate_absolute_and_relative_error_of_range(
        std::array<test_type, 1>{ norm }, std::array<test_type, 1> { norm_correct }
    );
    
    assert(absolute_error < 0.0001);
    assert(relative_error < 0.0001);

    return 0;
}
