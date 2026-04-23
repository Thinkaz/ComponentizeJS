export const noSnapshot = true;

export async function test(instance) {
  const results = instance.source.searchNovels("dune", 0);
  if (results.length !== 1) throw new Error(`Expected 1, got ${results.length}`);
  if (results[0].title !== "Dune") throw new Error(`Wrong: ${results[0].title}`);
}
