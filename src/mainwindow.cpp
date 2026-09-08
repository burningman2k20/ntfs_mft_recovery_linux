#include "mainwindow.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QApplication>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), recoverer_(std::make_unique<NTFSRecoverer>()) {
    
    resize(900, 600);
    setWindowTitle("NTFS MFT Deep Parser & Data Recovery (Linux)");

    auto* central_widget = new QWidget(this);
    auto* layout = new QVBoxLayout(central_widget);

    auto* btn_layout = new QHBoxLayout();
    btn_open_ = new QPushButton("Open Device / Image...", this);
    btn_recover_ = new QPushButton("Recover Selected", this);
    btn_recover_->setEnabled(false);

    btn_layout->addWidget(btn_open_);
    btn_layout->addWidget(btn_recover_);
    layout->addLayout(btn_layout);

    tree_view_ = new QTreeView(this);
    tree_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({"Name", "Size (Bytes)", "Record ID"});
    tree_view_->setModel(model_);

    layout->addWidget(tree_view_);
    setCentralWidget(central_widget);

    connect(btn_open_, &QPushButton::clicked, this, &MainWindow::open_image);
    connect(btn_recover_, &QPushButton::clicked, this, &MainWindow::recover_selected);
}

void MainWindow::open_image() {
    QString file_path = QFileDialog::getOpenFileName(this, "Select Raw Image / Volume");
    if (file_path.isEmpty()) return;

    if (!recoverer_->open_device(file_path.toStdString())) {
        QMessageBox::critical(this, "Error", "Failed to open device/file. Run with sudo if accessing raw partitions.");
        return;
    }

    QProgressDialog progress("Scanning volume for MFT records...", "Cancel", 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.show();

    recoverer_->scan_mft([&progress](size_t found) {
        progress.setLabelText(QString("Scanning... %1 records found").arg(found));
        QApplication::processEvents();
    });

    progress.close();
    populate_tree();
    btn_recover_->setEnabled(true);
}

void MainWindow::add_tree_node(QStandardItem* parent_item, uint64_t parent_id) {
    const auto& tree = recoverer_->get_tree();
    const auto& records = recoverer_->get_records();

    if (!tree.count(parent_id)) return;

    for (uint64_t child_id : tree.at(parent_id)) {
        if (!records.count(child_id)) continue;
        const auto& rec = records.at(child_id);

        auto* name_item = new QStandardItem(QString::fromStdString(rec.filename));
        name_item->setData(static_cast<qulonglong>(rec.record_num), Qt::UserRole);

        auto* size_item = new QStandardItem(rec.is_directory ? "<DIR>" : QString::number(rec.file_size));
        auto* id_item = new QStandardItem(QString::number(rec.record_num));

        parent_item->appendRow({name_item, size_item, id_item});

        if (rec.is_directory && child_id != parent_id) {
            add_tree_node(name_item, child_id);
        }
    }
}

void MainWindow::populate_tree() {
    model_->removeRows(0, model_->rowCount());
    add_tree_node(model_->invisibleRootItem(), 5); // 5 is Root Directory
}

void MainWindow::recover_selected() {
    auto selected_indexes = tree_view_->selectionModel()->selectedRows();
    if (selected_indexes.isEmpty()) return;

    QString dest_dir = QFileDialog::getExistingDirectory(this, "Select Target Extraction Directory");
    if (dest_dir.isEmpty()) return;

    std::filesystem::path base_path = dest_dir.toStdString();

    // 1. Collect all selected record IDs
    std::vector<uint64_t> selected_ids;
    for (const auto& idx : selected_indexes) {
        selected_ids.push_back(idx.data(Qt::UserRole).toULongLong());
    }

    // 2. Gather all tasks and files to count total items
    std::vector<RecoveryItem> tasks;
    for (uint64_t rec_id : selected_ids) {
        recoverer_->collect_items(rec_id, base_path, tasks);
    }

    if (tasks.empty()) {
        QMessageBox::warning(this, "Warning", "No valid files or folders found to recover.");
        return;
    }

    // 3. Setup Progress Dialog
    QProgressDialog progress("Initializing extraction...", "Cancel", 0, static_cast<int>(tasks.size()), this);
    progress.setWindowTitle("Recovering Files");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.resize(550, 140);

    size_t recovered_count = 0;
    bool was_cancelled = false;

    // 4. Perform extraction with live UI updates
    for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
        if (progress.wasCanceled()) {
            was_cancelled = true;
            break;
        }

        const auto& item = tasks[i];
        QString item_type = item.is_directory ? "[DIR]" : "[FILE]";
        QString current_name = QString::fromStdString(item.target_path.filename().string());

        QString label_text = QString("Recovering (%1/%2):\n%3 %4")
                                 .arg(i + 1)
                                 .arg(tasks.size())
                                 .arg(item_type)
                                 .arg(current_name);

        progress.setLabelText(label_text);
        progress.setValue(i);
        QApplication::processEvents();

        if (item.is_directory) {
            std::filesystem::create_directories(item.target_path);
        } else {
            if (recoverer_->extract_file(item.record, item.target_path)) {
                recovered_count++;
            }
        }
    }

    progress.setValue(static_cast<int>(tasks.size()));

    // 5. Completion notice
    if (was_cancelled) {
        QMessageBox::warning(this, "Cancelled", QString("Recovery aborted by user.\n%1 file(s) recovered.").arg(recovered_count));
    } else {
        QMessageBox::information(this, "Complete", QString("Successfully recovered %1 item(s).").arg(recovered_count));
    }
}