#include <algorithm>
#include <iostream>
#include <list>
#include <numeric>
#include <random>
#include <vector>
#include "cnn/metric-util.h"
#include <initializer_list>
#include <Eigen/LU>

using namespace std;
namespace cnn { namespace metric {

    cnn::real cosine_similarity(const std::vector<cnn::real> &s1, const std::vector<cnn::real> &s2)
    {
        cnn::real flt = 0.0;
        cnn::real flt1 = 0.0, flt2 = 0.0;

        for (int k = 0; k < s1.size(); k++)
        {
            flt += s1[k] * s2[k];
            flt1 += s1[k] * s1[k];
            flt2 += s2[k] * s2[k];
        }

        if (flt1 == 0.0) flt1 = FLT_EPSILON;
        if (flt2 == 0.0) flt2 = FLT_EPSILON;
        flt1 = sqrt(flt1);
        flt2 = sqrt(flt2);

        cnn::real val = flt / (flt1 * flt2);

        return val;
    }

    int levenshtein_distance(const vector<std::string> &s1, const vector<std::string> &s2)
    {
        // To change the type this function manipulates and returns, change
        // the return type and the types of the two variables below.
        int s1len = s1.size();
        int s2len = s2.size();

        auto column_start = (decltype(s1len))1;

        auto column = new decltype(s1len)[s1len + 1];
        std::iota(column + column_start, column + s1len + 1, column_start);

        for (auto x = column_start; x <= s2len; x++) {
            column[0] = x;
            auto last_diagonal = x - column_start;
            for (auto y = column_start; y <= s1len; y++) {
                auto old_diagonal = column[y];
                auto possibilities = {
                    column[y] + 1,
                    column[y - 1] + 1,
                    last_diagonal + (s1[y - 1] == s2[x - 1] ? 0 : 1)
                };
                column[y] = std::min(possibilities);
                last_diagonal = old_diagonal;
            }
        }
        auto result = column[s1len];
        delete[] column;
        return result;
    }
} }
