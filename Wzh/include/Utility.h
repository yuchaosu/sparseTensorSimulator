#ifndef UTILITY_H
#define UTILITY_H

#include <iostream>
#include <limits>
#include <cmath>
#include <sstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <tuple>
#include <fstream>

constexpr int INVALID_INT = std::numeric_limits<int>::min();
constexpr double INVALID_DOUBLE = std::numeric_limits<double>::quiet_NaN();

#include <limits>

struct DataPackage {
    double value;
    int index1;
    int index2;

    DataPackage() 
        : value(INVALID_DOUBLE), index1(INVALID_INT), index2(INVALID_INT) {}

    DataPackage(double v, int i1, int i2) 
        : value(v), index1(i1), index2(i2) {}

    friend std::ostream& operator<<(std::ostream& os, const DataPackage& dp) {
        os << "(" << dp.value << ", " << dp.index1 << ", " << dp.index2 << ")";
        return os;
    }

    bool isValid() const {
        return !std::isnan(value) && index1 != INVALID_INT && index2 != INVALID_INT;
    }
};




double random_double(double min_val, double max_val);
std::vector<std::vector<double>> generate_random_matrix(int size, const std::vector<int>& offsets, double min_val = 0.0, double max_val = 10.0);
std::vector<std::vector<int>> generate_symmetric_offsets(int range_size);
std::unordered_map<int, std::vector<std::tuple<double, int, int>>> extract_diagonals(const std::vector<std::vector<double>>& matrix, const std::vector<int>& offsets);
std::vector<int> computeResultDiagonals(const std::vector<int>& A_diags, const std::vector<int>& B_diags, int size);
std::vector<std::vector<double>> diagonals_to_dense(int size, const std::map<int, std::vector<double>>& diagonals);
std::vector<std::vector<double>> dense_matrix_multiply(const std::vector<std::vector<double>>& A, const std::vector<std::vector<double>>& B);
void print_matrix(const std::vector<std::vector<double>>& matrix, std::ofstream& out);
std::vector<std::vector<DataPackage>> buildDatapackage(std::map<int, std::vector<std::tuple<double, int, int>>>& diagonals);
int countMatrixMismatches(const std::vector<std::vector<double>>& ref, const std::vector<std::vector<double>>& sim, std::ofstream& out, double tolerance = 1e-6);
std::map<int, std::vector<double>> addMissingZeros(const std::map<int, std::vector<std::tuple<double, int, int>>>& diagonals, int size);
void show_progress_bar(int current, int total, int bar_width = 50);
std::vector<std::vector<std::unordered_map<int, std::vector<double>>>>
split_diagonals_by_group(const std::unordered_map<int, std::vector<double>>& diagonals,
                         int num_groups,
                         const std::string& mode,  // "row" or "col"
                         int diagonal_num) ;
// Helper to print grouped diagonals
void print_groups(const std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>>& grouped, std::ofstream& out);
std::pair<
    std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>>,
    std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>>
> split_double_diagonals_by_group(
    const std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diagonals_A,
    const std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diagonals_B,
    int num_groups, int diagonal_group_A, int diagonal_group_B);


std::vector<std::vector<double>> generate_random_vector(int size, double min_val = 0.0, double max_val = 10.0);
void print_vector(const std::vector<std::vector<double>>& vector, std::ofstream& out);
std::unordered_map<int, std::vector<std::tuple<double, int, int>>> rebuild_vector(const std::vector<std::vector<double>>& matrix);
std::vector<std::vector<double>> matrix_vector_multiply(const std::vector<std::vector<double>>& A, const std::vector<std::vector<double>>& B);
std::vector<int> rebuildOffsets(const std::map<int, std::vector<std::tuple<double, int, int>>>& diagonals);
std::vector<int> extractDiagonalOffsets(const std::string& filename);
std::unordered_map<int, std::vector<std::tuple<double, int, int>>>
createDiagonalMap(const std::string& filename, const std::vector<int>& diagonalOffsets, int matrixSize);
std::tuple<
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>,
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>,
    std::vector<int>, // cutting indices A
    std::vector<int>  // cutting indices B
>
split_double_diagonals_by_size(
    const std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diagonals_A,
    const std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diagonals_B,
    int max_group_size);
void saveDiagonalMatrixDense(
    const std::map<int, std::vector<double>>& diagonals,
    int size,
    const std::string& filename
);
std::unordered_map<int, std::vector<std::tuple<double, int, int>>> convertDiagonalMap(const std::map<int, std::vector<double>>& input, int size);
bool compareMatrices(const std::string& baselineFile, const std::string& testFile, int size, double epsilon = 1e-6);
std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>
splitDiagonals(const std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diagonals, int num);
std::pair<std::map<int, int>, int> splitMatrixDiagonals(int matrixSize, int diagonalsPerGroup);
#endif // UTILITY_H
