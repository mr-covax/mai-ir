# Токенизация

Извлечение текста из HTML, экспорт корпуса и токенизатор C++20.
Правила — в [TOKENIZATION.md](TOKENIZATION.md).

```bash
just build
just run page.txt
just test
just python-test
uv run --locked extract-text page.html --output page.txt
uv run --locked tokenize-corpus data/corpus --binary build/tokenize
```

`build/tokenize` читает файл или stdin, `--stats-only` выводит JSON со
статистикой. `build/tokenize_stream corpus.bin` обрабатывает упакованный корпус
одним процессом. Таблицы Unicode 15.0 хранятся в репозитории в `cpp/unicode_data.hpp`.

Google Test проверяет лексические правила, Unicode и неверный UTF-8;
Python-тесты проверяют извлечение, экспорт и обвязку корпуса; ответы внешнего
процесса подставляются, C++ бинарник из pytest не запускается.
Сохранённые замеры находятся в корневом `reports/3_tokenizer/`.
