#pragma once
#include <QMainWindow>
#include <QTreeView>
#include <QStandardItemModel>
#include <QPushButton>
#include <memory>
#include "recoverer.hpp"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void open_image();
    void recover_selected();

private:
    void populate_tree();
    void add_tree_node(QStandardItem* parent_item, uint64_t parent_id);

    QTreeView* tree_view_;
    QStandardItemModel* model_;
    QPushButton* btn_open_;
    QPushButton* btn_recover_;
    std::unique_ptr<NTFSRecoverer> recoverer_;
};