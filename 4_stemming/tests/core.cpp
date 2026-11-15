#include "error.hpp"
#include "lemma.hpp"
#include "stem.hpp"
#include <gtest/gtest.h>

TEST(Stemming, PorterRules) {
    const char* examples[][2] = {{"caresses", "caress"},
                                 {"ponies", "poni"},
                                 {"ties", "ti"},
                                 {"caress", "caress"},
                                 {"cats", "cat"},
                                 {"feed", "feed"},
                                 {"agreed", "agre"},
                                 {"plastered", "plaster"},
                                 {"bled", "bled"},
                                 {"motoring", "motor"},
                                 {"sing", "sing"},
                                 {"conflated", "conflat"},
                                 {"troubled", "troubl"},
                                 {"sized", "size"},
                                 {"hopping", "hop"},
                                 {"tanned", "tan"},
                                 {"falling", "fall"},
                                 {"hissing", "hiss"},
                                 {"fizzed", "fizz"},
                                 {"failing", "fail"},
                                 {"filing", "file"},
                                 {"happy", "happi"},
                                 {"sky", "sky"},
                                 {"relational", "relat"},
                                 {"conditional", "condit"},
                                 {"rational", "ration"},
                                 {"valenci", "valenc"},
                                 {"hesitanci", "hesit"},
                                 {"digitizer", "digit"},
                                 {"conformabli", "conform"},
                                 {"radicalli", "radic"},
                                 {"differentli", "differ"},
                                 {"vileli", "vile"},
                                 {"analogousli", "analog"},
                                 {"vietnamization", "vietnam"},
                                 {"predication", "predic"},
                                 {"operator", "oper"},
                                 {"feudalism", "feudal"},
                                 {"decisiveness", "decis"},
                                 {"hopefulness", "hope"},
                                 {"callousness", "callous"},
                                 {"formaliti", "formal"},
                                 {"sensitiviti", "sensit"},
                                 {"sensibiliti", "sensibl"},
                                 {"triplicate", "triplic"},
                                 {"formative", "form"},
                                 {"formalize", "formal"},
                                 {"electriciti", "electr"},
                                 {"electrical", "electr"},
                                 {"hopeful", "hope"},
                                 {"goodness", "good"},
                                 {"revival", "reviv"},
                                 {"allowance", "allow"},
                                 {"inference", "infer"},
                                 {"airliner", "airlin"},
                                 {"gyroscopic", "gyroscop"},
                                 {"adjustable", "adjust"},
                                 {"defensible", "defens"},
                                 {"irritant", "irrit"},
                                 {"replacement", "replac"},
                                 {"adjustment", "adjust"},
                                 {"dependent", "depend"},
                                 {"adoption", "adopt"},
                                 {"homologou", "homolog"},
                                 {"communism", "commun"},
                                 {"activate", "activ"},
                                 {"angulariti", "angular"},
                                 {"homologous", "homolog"},
                                 {"effective", "effect"},
                                 {"bowdlerize", "bowdler"},
                                 {"probate", "probat"},
                                 {"rate", "rate"},
                                 {"cease", "ceas"},
                                 {"controll", "control"},
                                 {"roll", "roll"},
                                 {"pthread_create", "pthread_create"},
                                 {"std::vector", "std::vector"},
                                 {"c++20", "c++20"},
                                 {"память", "память"},
                                 {"", ""}};

    for (const auto& example : examples) {
        SCOPED_TRACE(example[0]);
        char word[256];
        snprintf(word, sizeof(word), "%s", example[0]);
        ir::stem(word);
        EXPECT_STREQ(word, example[1]);
    }
}

TEST(Lemmatization, DictionaryAndInflections) {
    const char* examples[][2] = {{"children", "child"},
                                 {"mice", "mouse"},
                                 {"went", "go"},
                                 {"running", "run"},
                                 {"hoping", "hope"},
                                 {"uses", "use"},
                                 {"connections", "connection"},
                                 {"allocations", "allocation"},
                                 {"pthread_create", "pthread_create"},
                                 {"std::vector", "std::vector"},
                                 {"c++20", "c++20"},
                                 {"память", "память"},
                                 {"", ""}};

    for (const auto& example : examples) {
        SCOPED_TRACE(example[0]);
        char word[256];
        snprintf(word, sizeof(word), "%s", example[0]);
        ir::lemmatize(word, sizeof(word));
        EXPECT_STREQ(word, example[1]);
    }
}

TEST(Lemmatization, ExpandingWordCapacity) {
    char word[6] = "mice";
    ir::lemmatize(word, sizeof(word));
    EXPECT_STREQ(word, "mouse");

    char small[] = "mice";
    EXPECT_THROW(ir::lemmatize(small, sizeof(small)), ir::Error);
}
