export const source = {
  searchNovels(query, page) {
    const novels = [
      { title: "Dune", url: "/novel/dune", author: "Frank Herbert" },
      { title: "The Hobbit", url: "/novel/hobbit", author: "J.R.R. Tolkien" },
    ];
    return novels.filter(n => n.title.toLowerCase().includes(query.toLowerCase()));
  }
};
